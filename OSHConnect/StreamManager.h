#pragma once

#include <vector>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <functional>
#include <algorithm>
#include <utility>

#include "Websocket/StreamHandler.h"
#include "Websocket/StreamEventArgs.h"

namespace OSHConnect {
	// Forward declaration
	class OSHConnect;

	/// <summary>
	/// Manages data stream handlers and their associated data streams. All methods are thread-safe.
	/// </summary>
	class StreamManager {
	private:
		/// <summary>
		/// Data stream handlers created and owned by this manager.
		/// </summary>
		std::vector<std::unique_ptr<Websocket::StreamHandler>> ownedHandlers;

		/// <summary>
		/// Guards ownedHandlers and externalHandlers. Handlers are shut down outside it.
		/// </summary>
		mutable std::mutex mutex;

		/// <summary>
		/// External data stream handlers added to this manager (not owned).
		/// </summary>
		std::unordered_set<Websocket::StreamHandler*> externalHandlers;

		friend class OSHConnect;

		/// <summary>
		/// Package-private constructor, to be used by OSHConnect.
		/// </summary>
		StreamManager() = default;

	public:
		~StreamManager() {
			shutdown();
		}

		// Prevent copying
		StreamManager(const StreamManager&) = delete;
		StreamManager& operator=(const StreamManager&) = delete;

		/// <summary>
		/// Create a new data stream handler and add it to OSHConnect.
		/// No need to call addDataStreamHandler after calling this method.
		/// The manager takes ownership of the created handler.
		/// </summary>
		/// <param name="onStreamUpdate">The function to call when a data stream is updated.</param>
		/// <returns>Pointer to the created data stream handler.</returns>
		Websocket::StreamHandler* createDataStreamHandler(
			std::function<void(const Websocket::StreamEventArgs&)> onStreamUpdate) {

			// Create anonymous derived class
			class CallbackStreamHandler : public Websocket::StreamHandler {
			private:
				std::function<void(const Websocket::StreamEventArgs&)> callback;

			public:
				explicit CallbackStreamHandler(
					std::function<void(const Websocket::StreamEventArgs&)> callback)
					: callback(std::move(callback)) {
				}

				// Shut down before the callback is destroyed: the base class destructor runs after this class's
				// members are gone, while network threads could still be delivering updates
				~CallbackStreamHandler() override {
					shutdown();
				}

				void onStreamUpdate(const Websocket::StreamEventArgs& args) override {
					if (callback) {
						callback(args);
					}
				}
			};

			auto handler = std::make_unique<CallbackStreamHandler>(std::move(onStreamUpdate));
			auto* ptr = handler.get();
			std::lock_guard lock(mutex);
			ownedHandlers.push_back(std::move(handler));
			return ptr;
		}

		/// <summary>
		/// Add an external data stream handler to OSHConnect.
		/// This method is used to add a data stream handler created outside the OSHConnect instance.
		/// The caller maintains ownership of the handler.
		/// </summary>
		/// <param name="handler">The data stream handler to add.</param>
		void addDataStreamHandler(Websocket::StreamHandler* handler) {
			if (handler) {
				std::lock_guard lock(mutex);
				externalHandlers.insert(handler);
			}
		}

		/// <summary>
		/// Get a list of all data stream handlers associated with OSHConnect.
		/// This includes both owned and external handlers.
		/// </summary>
		/// <returns>A vector of pointers to data stream handlers.</returns>
		std::vector<Websocket::StreamHandler*> getDataStreamHandlers() const {
			std::vector<Websocket::StreamHandler*> result;
			std::lock_guard lock(mutex);

			// Add owned handlers
			for (const auto& handler : ownedHandlers) {
				result.push_back(handler.get());
			}

			// Add external handlers
			for (auto* handler : externalHandlers) {
				result.push_back(handler);
			}

			return result;
		}

		/// <summary>
		/// Shutdown the data stream handler and its associated data streams,
		/// and remove it from OSHConnect.
		/// If the handler was created by this manager, it will be destroyed; when called from the handler's own
		/// onStreamUpdate, that happens as soon as onStreamUpdate returns.
		/// </summary>
		/// <param name="handler">The handler to shutdown and remove.</param>
		void shutdownDataStreamHandler(Websocket::StreamHandler* handler) {
			if (!handler) return;

			// Take the handler out of the manager, then shut it down (and destroy it, if owned) outside the lock
			std::unique_ptr<Websocket::StreamHandler> owned;
			{
				std::lock_guard lock(mutex);
				externalHandlers.erase(handler);
				auto it = std::find_if(ownedHandlers.begin(), ownedHandlers.end(),
					[handler](const std::unique_ptr<Websocket::StreamHandler>& h) { return h.get() == handler; });
				if (it != ownedHandlers.end()) {
					owned = std::move(*it);
					ownedHandlers.erase(it);
				}
			}
			release(std::move(owned), handler);
		}

		/// <summary>
		/// Shutdown all data stream handlers and disconnect from all data streams,
		/// and remove them from OSHConnect.
		/// </summary>
		void shutdownDataStreamHandlers() {
			// Take every handler out of the manager, then shut them down outside the lock
			std::vector<std::unique_ptr<Websocket::StreamHandler>> owned;
			std::unordered_set<Websocket::StreamHandler*> external;
			{
				std::lock_guard lock(mutex);
				owned.swap(ownedHandlers);
				external.swap(externalHandlers);
			}

			// Shutdown and destroy owned handlers
			for (auto& handler : owned) {
				auto* raw = handler.get();
				release(std::move(handler), raw);
			}

			// Shutdown external handlers (but don't delete them)
			for (auto* handler : external) {
				handler->shutdown();
			}
		}

	private:
		/// <summary>
		/// Shuts the handler down, then destroys it if owned. When called from the handler's own onStreamUpdate,
		/// the handler can't be destroyed yet, since that callback is still running on it: its delivery thread
		/// destroys it once the callback returns.
		/// </summary>
		static void release(std::unique_ptr<Websocket::StreamHandler> owned, Websocket::StreamHandler* handler) {
			const bool fromOwnUpdate = handler->isDeliveryThread();
			handler->shutdown();
			if (owned && fromOwnUpdate) {
				auto* raw = owned.release();
				raw->runAfterCurrentUpdate([raw] { delete raw; });
			}
		}

		/// <summary>
		/// Shutdown all data stream handlers.
		/// Package-private method called by OSHConnect.
		/// </summary>
		void shutdown() {
			shutdownDataStreamHandlers();
		}
	};

}
