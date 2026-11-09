#pragma once

#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <utility>
#include <stdexcept>
#include <optional>

#include <CSAPI/DataModels/TimeExtent.h>

#include "SerialExecutor.h"
#include "StreamListener.h"
#include "StreamEventArgs.h"
#include "StreamStatus.h"
#include "../OSHDataStream.h"
#include "../Constants/RequestFormat.h"

namespace OSHConnect {
	class StreamManager;
}

namespace OSHConnect::Websocket {
	/// <summary>
	/// A handler for multiple data streams.
	/// Override the onStreamUpdate method to receive data from the data streams.
	/// This class manages multiple StreamListener instances and can time-synchronize their events.
	/// </summary>
	/// <remarks>
	/// Threading: all methods are thread-safe. onStreamUpdate is called on the handler's own delivery thread,
	/// one update at a time, in the order the updates arrived; the network threads only queue them.
	/// Because that thread is not a network thread, onStreamUpdate may call any method of the handler or its
	/// listeners, including shutdown(); StreamManager::shutdownDataStreamHandler may also be called from it,
	/// and destroys the handler once onStreamUpdate returns.
	/// Keep onStreamUpdate short (in Unreal, hand the data to the game thread): updates queue up while it runs.
	/// A class deriving from StreamHandler must call shutdown() in its own destructor, so updates stop
	/// before the derived class's members are destroyed.
	/// </remarks>
	class StreamHandler {
	private:
		// Internal listener class that forwards events to the handler
		class ManagedStreamListener : public StreamListener {
		private:
			StreamHandler* handler;

		public:
			ManagedStreamListener(std::shared_ptr<OSHDataStream> dataStream, StreamHandler* handler)
				: StreamListener(std::move(dataStream)), handler(handler) {
			}

			void onStreamUpdate(const StreamEventArgs& args) override {
				// Forward to handler (could be time-synchronized in the future)
				if (handler) {
					handler->dispatchStreamUpdate(args);
				}
			}
		};

		// Guards dataStreamListeners and the settings below. Never held while connecting or delivering updates.
		mutable std::mutex mutex;
		// Shared so a listener being connected can't be destroyed by a concurrent removal
		std::vector<std::shared_ptr<ManagedStreamListener>> dataStreamListeners;
		std::optional<Constants::RequestFormat> requestFormat;
		std::optional<ConnectedSystemsAPI::DataModels::TimeExtent> timeExtent;
		double replaySpeed = 1.0;
		StreamStatus status = StreamStatus::DISCONNECTED;

		// Delivers onStreamUpdate one update at a time, off the listeners' network threads
		SerialExecutor dispatcher;
		std::atomic<bool> acceptingUpdates{ true };

		// Called on a listener's network thread: only queues the update, so it never waits on user code
		void dispatchStreamUpdate(const StreamEventArgs& args) {
			if (!acceptingUpdates) return;
			dispatcher.post([this, args] {
				if (acceptingUpdates) onStreamUpdate(args);
			});
		}

		/// <summary>
		/// Runs the function once the update being delivered returns, on the delivery thread.
		/// Used by StreamManager to destroy a handler shut down from its own onStreamUpdate.
		/// </summary>
		void runAfterCurrentUpdate(std::function<void()> function) {
			dispatcher.runOnExit(std::move(function));
		}

		friend class ::OSHConnect::StreamManager;

		std::vector<std::shared_ptr<ManagedStreamListener>> listenersSnapshot() const {
			std::lock_guard lock(mutex);
			return dataStreamListeners;
		}

		/// <summary>
		/// Removes the listeners matching the predicate, then disconnects and releases them outside the lock.
		/// </summary>
		template<typename Predicate>
		bool removeListeners(Predicate matches) {
			std::vector<std::shared_ptr<ManagedStreamListener>> removed;
			{
				std::lock_guard lock(mutex);
				auto it = std::stable_partition(dataStreamListeners.begin(), dataStreamListeners.end(),
					[&matches](const std::shared_ptr<ManagedStreamListener>& listener) { return !matches(listener); });
				removed.assign(it, dataStreamListeners.end());
				dataStreamListeners.erase(it, dataStreamListeners.end());
			}
			for (auto& listener : removed) {
				listener->disconnect();
			}
			return !removed.empty();
		}

	public:
		StreamHandler() = default;
		virtual ~StreamHandler() {
			shutdown();
		}

		// Prevent copying
		StreamHandler(const StreamHandler&) = delete;
		StreamHandler& operator=(const StreamHandler&) = delete;

		/// <summary>
		/// Virtual method called when a data stream receives an update.
		/// Override this method in derived classes to handle the incoming data.
		/// Called on the handler's delivery thread, one update at a time; see the class remarks.
		/// </summary>
		/// <param name="args">The event arguments containing the data and metadata.</param>
		virtual void onStreamUpdate(const StreamEventArgs& args) {
			// Override this in derived classes
		}

		/// <summary>
		/// Connects to all data streams.
		/// </summary>
		void connect() {
			std::vector<std::shared_ptr<ManagedStreamListener>> listeners;
			{
				std::lock_guard lock(mutex);
				if (status == StreamStatus::SHUTDOWN) {
					throw std::runtime_error("Handler has been shut down.");
				}
				listeners = dataStreamListeners;
			}

			for (auto& listener : listeners) {
				listener->connect();
			}

			std::lock_guard lock(mutex);
			if (status != StreamStatus::SHUTDOWN) {
				status = StreamStatus::CONNECTED;
			}
		}

		/// <summary>
		/// Disconnects from all data streams.
		/// </summary>
		void disconnect() {
			for (auto& listener : listenersSnapshot()) {
				listener->disconnect();
			}

			std::lock_guard lock(mutex);
			if (status != StreamStatus::SHUTDOWN) {
				status = StreamStatus::DISCONNECTED;
			}
		}

		/// <summary>
		/// Shuts down the data stream handler.
		/// This will disconnect from all data streams and remove them from the handler.
		/// The handler will no longer be usable after this method is called.
		/// </summary>
		void shutdown() {
			acceptingUpdates = false;
			{
				std::lock_guard lock(mutex);
				status = StreamStatus::SHUTDOWN;
			}
			shutdownAllDataStreamListeners();
			// Waits for the update being delivered, unless called from it
			dispatcher.stop();
		}

		/// <summary>
		/// True if called from inside onStreamUpdate.
		/// </summary>
		bool isDeliveryThread() const {
			return dispatcher.isCurrentThread();
		}

		/// <summary>
		/// Adds a data stream to the handler.
		/// Also connects to the data stream if the handler is already connected.
		/// If the data stream is already in the handler, returns the existing listener.
		/// </summary>
		/// <param name="dataStream">The data stream to add. The handler shares ownership of it.</param>
		/// <returns>Pointer to the StreamListener managing this data stream, valid until the listener is removed
		/// or the handler shut down; nullptr if the data stream is null.</returns>
		StreamListener* addDataStreamListener(const std::shared_ptr<OSHDataStream>& dataStream) {
			if (!dataStream) return nullptr;

			std::shared_ptr<ManagedStreamListener> listener;
			bool connectNow = false;
			{
				// Find or add the listener in one step, so concurrent calls can't add it twice
				std::lock_guard lock(mutex);
				for (auto& existing : dataStreamListeners) {
					if (existing->getDataStream()->getId() == dataStream->getId()) {
						return existing.get();
					}
				}

				listener = std::make_shared<ManagedStreamListener>(dataStream, this);

				// Apply current settings
				if (requestFormat.has_value()) {
					listener->setRequestFormat(requestFormat.value());
				}
				listener->setReplaySpeed(replaySpeed);
				if (timeExtent.has_value()) {
					listener->setTimeExtent(timeExtent.value());
				}

				dataStreamListeners.push_back(listener);
				connectNow = status == StreamStatus::CONNECTED;
			}

			// Connect if handler is already connected, outside the lock since connecting blocks.
			// If it fails, the listener is not kept, as before.
			if (connectNow) {
				try {
					listener->connect();
				}
				catch (...) {
					removeListeners([&listener](const std::shared_ptr<ManagedStreamListener>& l) { return l == listener; });
					throw;
				}
			}

			return listener.get();
		}

		/// <summary>
		/// Disconnects from the data stream and removes it from the handler.
		/// </summary>
		/// <param name="dataStream">The data stream to remove.</param>
		/// <returns>True if the listener was shut down and removed, false if not found.</returns>
		bool shutdownDataStreamListener(const OSHDataStream& dataStream) {
			const auto id = dataStream.getId();
			return removeListeners([&id](const std::shared_ptr<ManagedStreamListener>& listener) {
				return listener->getDataStream()->getId() == id;
			});
		}

		/// <summary>
		/// Disconnects from the data stream and removes it from the handler.
		/// </summary>
		/// <param name="listener">The listener to remove.</param>
		/// <returns>True if the listener was removed, false if not found.</returns>
		bool shutdownDataStreamListener(StreamListener* listener) {
			if (!listener) return false;
			return removeListeners([listener](const std::shared_ptr<ManagedStreamListener>& l) {
				return l.get() == listener;
			});
		}

		/// <summary>
		/// Shuts down all data streams and removes them from the handler.
		/// </summary>
		void shutdownAllDataStreamListeners() {
			removeListeners([](const std::shared_ptr<ManagedStreamListener>&) { return true; });
		}

		/// <summary>
		/// Get the number of data stream listeners in the handler.
		/// </summary>
		size_t getDataStreamListenerCount() const {
			std::lock_guard lock(mutex);
			return dataStreamListeners.size();
		}

		/// <summary>
		/// The format of the request.
		/// If nullopt, the format will not be specified in the request.
		/// </summary>
		std::optional<Constants::RequestFormat> getRequestFormat() const {
			std::lock_guard lock(mutex);
			return requestFormat;
		}

		/// <summary>
		/// Sets the format of the request for all listeners.
		/// Calling this will reconnect any connected listeners.
		/// </summary>
		void setRequestFormat(Constants::RequestFormat format) {
			{
				std::lock_guard lock(mutex);
				requestFormat = format;
			}
			for (auto& listener : listenersSnapshot()) {
				listener->setRequestFormat(format);
			}
		}

		/// <summary>
		/// The time period for the data streams.
		/// If nullopt, will listen to the data streams in real-time.
		/// </summary>
		std::optional<ConnectedSystemsAPI::DataModels::TimeExtent> getTimeExtent() const {
			std::lock_guard lock(mutex);
			return timeExtent;
		}

		/// <summary>
		/// Sets the time period for all listeners.
		/// Calling this will reconnect any connected listeners.
		/// </summary>
		void setTimeExtent(const ConnectedSystemsAPI::DataModels::TimeExtent& extent) {
			{
				std::lock_guard lock(mutex);
				timeExtent = extent;
			}
			for (auto& listener : listenersSnapshot()) {
				listener->setTimeExtent(extent);
			}
		}

		/// <summary>
		/// The replay speed for the data streams.
		/// Only applicable for historical data streams.
		/// 1.0 is default speed, 0.1 is 10x slower, 10.0 is 10x faster.
		/// </summary>
		double getReplaySpeed() const {
			std::lock_guard lock(mutex);
			return replaySpeed;
		}

		/// <summary>
		/// Sets the replay speed for all listeners.
		/// Calling this will reconnect any connected listeners.
		/// </summary>
		void setReplaySpeed(double speed) {
			{
				std::lock_guard lock(mutex);
				replaySpeed = speed;
			}
			for (auto& listener : listenersSnapshot()) {
				listener->setReplaySpeed(speed);
			}
		}

		/// <summary>
		/// The status of the data stream handler.
		/// </summary>
		StreamStatus getStatus() const {
			std::lock_guard lock(mutex);
			return status;
		}
	};

}
