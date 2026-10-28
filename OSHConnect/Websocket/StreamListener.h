#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <regex>
#include <cstring>
#include <cstdio>
#include <vector>
#include <functional>
#include <exception>
#include <iostream>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <memory>

#include <ixwebsocket/IXWebSocket.h>
#include <CSAPI/DataModels/TimeExtent.h>
#include <nlohmann/json.hpp>

#include "StreamStatus.h"
#include "StreamEventArgs.h"
#include "../OSHDataStream.h"
#include "../OSHSystem.h"
#include "../OSHNode.h"
#include "../Constants/RequestFormat.h"
#include "../Util/QueryStringBuilder.h"

namespace OSHConnect::Websocket {
	/// <summary>
	/// Log levels for the logger callback.
	/// </summary>
	enum class LogLevel {
		DEBUG,
		INFO,
		WARNING,
		ERR
	};

	/// <summary>
	/// Callback type for logging.
	/// </summary>
	using LogCallback = std::function<void(const std::string&, LogLevel)>;

	/// <summary>
	/// Callback type for status change notifications.
	/// </summary>
	using StatusListener = std::function<void(StreamStatus)>;

	/// <summary>
	/// Listener for a single data stream.
	/// Override the onStreamUpdate method to handle the data received from the data stream.
	/// To listen to multiple data streams, use a StreamHandler.
	/// This class is thread-safe.
	/// </summary>
	class StreamListener {
	private:
		static inline const std::regex dateRegexText{ R"(\d{4}-\d{2}-\d{2}T\d{2}:)" };
		static inline const std::regex dateRegexXML{ R"(<[^>]+>(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z)</[^>]+>)" };

		OSHDataStream dataStream;

		// Configuration mutex protects: requestFormat, timeExtent, replaySpeed, connectionTimeout, maxReconnectAttempts
		mutable std::mutex configMutex;
		std::optional<Constants::RequestFormat> requestFormat;
		std::optional<ConnectedSystemsAPI::DataModels::TimeExtent> timeExtent;
		double replaySpeed = 1.0;
		std::chrono::seconds connectionTimeout{ 30 };
		std::atomic<bool> autoReconnect{ false };
		int maxReconnectAttempts = 3;

		// WebSocket mutex protects: webSocket and connection operations
		mutable std::mutex wsMutex;
		ix::WebSocket webSocket;

		// Status listener mutex protects: statusListeners, nextListenerId
		mutable std::mutex listenerMutex;
		using ListenerId = size_t;
		std::unordered_map<ListenerId, StatusListener> statusListeners;
		ListenerId nextListenerId = 0;

		// Logger mutex protects: logger
		mutable std::mutex loggerMutex;
		std::optional<LogCallback> logger;

		// Atomic state flags
		std::atomic<bool> connected{ false };
		std::atomic<bool> isShutdown{ false };

		// Connection state synchronization
		mutable std::mutex connectionMutex;
		std::condition_variable connectionCV;
		bool connectionAttemptComplete = false;

		// Reconnection management
		mutable std::mutex reconnectMutex;
		std::unique_ptr<std::thread> reconnectThread;
		std::atomic<bool> reconnectionInProgress{ false };

		/// <summary>
		/// Logs a message using the configured logger callback.
		/// </summary>
		/// <param name="message">The message to log.</param>
		/// <param name="level">The log level.</param>
		void log(const std::string& message, LogLevel level) const {
			std::lock_guard lock(loggerMutex);
			if (logger.has_value()) {
				try {
					(*logger)(message, level);
				}
				catch (...) {
					// Ignore exceptions in logger to prevent cascading failures
				}
			}
			else {
				// Fallback to stderr for errors
				if (level == LogLevel::ERR || level == LogLevel::WARNING) {
					std::cerr << "[" << (level == LogLevel::ERR ? "ERROR" : "WARNING") << "] " << message << std::endl;
				}
			}
		}

		/// <summary>
		/// Validates the configuration before connecting.
		/// </summary>
		void validateConfiguration() const {
			if (dataStream.getId().empty()) {
				throw std::invalid_argument("DataStream ID cannot be empty");
			}

			auto const* system = dataStream.getParentSystem();
			if (system == nullptr) {
				throw std::invalid_argument("DataStream parent system is null");
			}

			auto const* node = system->getParentNode();
			if (node == nullptr) {
				throw std::invalid_argument("System parent node is null");
			}

			if (node->getWSPrefix().empty()) {
				throw std::invalid_argument("WebSocket prefix is empty");
			}

			if (node->getApiEndpoint().empty()) {
				throw std::invalid_argument("API endpoint is empty");
			}

			std::lock_guard lock(configMutex);
			if (replaySpeed <= 0.0) {
				log("Replay speed is zero or negative, no data will be received", LogLevel::WARNING);
			}
		}

		std::string buildRequestString() {
			std::lock_guard lock(configMutex);
			Util::QueryStringBuilder builder;

			if (requestFormat.has_value()) {
				builder.withParameter("format", getMimeType(requestFormat.value()));
			}

			if (timeExtent.has_value() && !timeExtent->isNow()) {
				builder.withParameter("phenomenonTime", timeExtent.value());
				builder.withParameter("replaySpeed", replaySpeed);
			}

			return builder.build();
		}

		/// <summary>
		/// Determines the format of the request based on the data.
		/// </summary>
		/// <param name="data">The data received from the data stream.</param>
		/// <returns>The format of the request.</returns>
		Constants::RequestFormat determineRequestFormat(const std::vector<uint8_t>& data) const {
			{
				std::lock_guard lock(configMutex);
				if (requestFormat.has_value()) {
					return requestFormat.value();
				}
			}

			if (data.empty()) {
				return Constants::RequestFormat::JSON;
			}

			if (data[0] == '{') {
				return Constants::RequestFormat::JSON;
			}
			else if (data[0] == '<') {
				return Constants::RequestFormat::SWE_XML;
			}
			else {
				// Check if the first 14 characters are a date
				if (data.size() >= 14) {
					std::string dataString(data.begin(), data.begin() + 14);
					if (std::regex_search(dataString, dateRegexText)) {
						return Constants::RequestFormat::PLAIN_TEXT;
					}
				}
				return Constants::RequestFormat::SWE_BINARY;
			}
		}

		/// <summary>
		/// Determines the timestamp of the data.
		/// </summary>
		/// <param name="format">The format of the request.</param>
		/// <param name="data">The data received from the data stream.</param>
		/// <returns>The timestamp of the data in milliseconds since epoch, or -1 if parsing fails.</returns>
		int64_t determineTimestamp(Constants::RequestFormat format, const std::vector<uint8_t>& data) const {
			try {
				if (format == Constants::RequestFormat::JSON ||
					format == Constants::RequestFormat::OM_JSON ||
					format == Constants::RequestFormat::SWE_JSON) {
					// Parse JSON to get phenomenonTime
					std::string jsonStr(data.begin(), data.end());
					auto json = nlohmann::json::parse(jsonStr);
					if (json.contains("phenomenonTime")) {
						std::string phenomenonTime = json["phenomenonTime"].get<std::string>();
						// Parse ISO 8601 timestamp
						return parseISO8601(phenomenonTime);
					}
				}
				else if (format == Constants::RequestFormat::SWE_XML) {
					// Get the timestamp from the first date in the XML
					std::string xml(data.begin(), data.end());
					std::smatch match;
					if (std::regex_search(xml, match, dateRegexXML)) {
						if (match.size() > 1) {
							std::string date = match[1].str();
							return parseISO8601(date);
						}
					}
				}
				else if (format == Constants::RequestFormat::SWE_CSV ||
					format == Constants::RequestFormat::PLAIN_TEXT) {
					// Get the timestamp from the first element of the CSV
					std::string text(data.begin(), data.end());
					size_t commaPos = text.find(',');
					if (commaPos != std::string::npos) {
						std::string timestampStr = text.substr(0, commaPos);
						return parseISO8601(timestampStr);
					}
					else {
						return parseISO8601(text);
					}
				}
				else if (format == Constants::RequestFormat::SWE_BINARY) {
					// Get the timestamp from the first 8 bytes of the binary data
					if (data.size() >= 8) {
						double timestampDouble;
						std::memcpy(&timestampDouble, data.data(), sizeof(double));
						return static_cast<int64_t>(timestampDouble * 1000);
					}
				}
			}
			catch (...) {
				// Return -1 on any parsing error
				return -1;
			}

			return -1;
		}

		/// <summary>
		/// Parse ISO 8601 timestamp to milliseconds since epoch.
		/// </summary>
		/// <param name="timestamp">ISO 8601 formatted timestamp string.</param>
		/// <returns>Milliseconds since Unix epoch, or -1 if parsing fails.</returns>
		int64_t parseISO8601(const std::string& timestamp) const {
			std::tm tm = {};
			int millis = 0;

			// Try to parse with milliseconds
#ifdef _WIN32
			// Use sscanf_s on Windows (secure version)
			if (sscanf_s(timestamp.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
				&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
				&tm.tm_hour, &tm.tm_min, &tm.tm_sec, &millis) >= 6) {
#else
			// Use standard sscanf on other platforms
			if (std::sscanf(timestamp.c_str(), "%d-%d-%dT%d:%d:%d.%dZ",
				&tm.tm_year, &tm.tm_mon, &tm.tm_mday,
				&tm.tm_hour, &tm.tm_min, &tm.tm_sec, &millis) >= 6) {
#endif

				tm.tm_year -= 1900;
				tm.tm_mon -= 1;

				// Convert to time_t treating tm as UTC
#ifdef _WIN32
				auto time = _mkgmtime(&tm);  // Windows
#else
				auto time = timegm(&tm);     // POSIX
#endif

				if (time == -1) return -1;

				return time * 1000 + millis;
			}

			return -1;
		}

		/// <summary>
		/// Internal connect implementation (not thread-safe, must be called with wsMutex held).
		/// </summary>
		void connectInternal() {
			if (isShutdown) {
				throw std::runtime_error("Listener has been shut down.");
			}

			// Validate before attempting connection
			validateConfiguration();

			// Ensure clean state before connecting
			disconnectInternal();

			// Reset connection state
			{
				std::lock_guard lock(connectionMutex);
				connectionAttemptComplete = false;
			}

			try {
				auto* node = dataStream.getParentSystem()->getParentNode();

				// Build the full WebSocket URL
				std::string fullUrl = node->getWSPrefix() +
					node->getApiEndpoint() +
					"/" + Constants::getEndpoint(Constants::Service::DATASTREAMS) + "/" +
					dataStream.getId() + "/" +
					Constants::getEndpoint(Constants::Service::OBSERVATIONS) +
					buildRequestString();

				log("Connecting to: " + fullUrl, LogLevel::INFO);

				// Configure the WebSocket
				webSocket.setUrl(fullUrl);

				// Authenticate the handshake with the same credentials as the node's REST requests
				ix::WebSocketHttpHeaders headers;
				if (!node->getAuthenticationToken().empty()) {
					headers["Authorization"] = node->getConSysAPI().getAuthHeader();
				}
				webSocket.setExtraHeaders(headers);

				// Enable automatic ping for connection health monitoring
				webSocket.setPingInterval(30);  // Send ping every 30 seconds

				// Set connection timeout
				std::chrono::seconds timeout;
				{
					std::lock_guard lock(configMutex);
					timeout = connectionTimeout;
				}
				webSocket.setHandshakeTimeout(static_cast<int>(timeout.count())); // seconds

				// Set up event handlers
				webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
					// Early exit if shutting down to prevent use-after-free
					if (isShutdown) {
						return;
					}

					if (msg->type == ix::WebSocketMessageType::Message) {
						// Handle binary or text message
						const std::string& payload = msg->str;
						std::vector<uint8_t> data(payload.begin(), payload.end());

						if (!data.empty()) {
							try {
								onStreamUpdate(data);
							}
							catch (const std::exception& e) {
								log("Error in onStreamUpdate callback: " + std::string(e.what()), LogLevel::ERR);
							}
							catch (...) {
								log("Unknown error in onStreamUpdate callback", LogLevel::ERR);
							}
						}
					}
					else if (msg->type == ix::WebSocketMessageType::Open) {
						connected = true;
						log("Successfully connected", LogLevel::INFO);

						// Notify connection waiters
						{
							std::lock_guard lock(connectionMutex);
							connectionAttemptComplete = true;
						}
						connectionCV.notify_all();

						updateStatus(StreamStatus::CONNECTED);
					}
					else if (msg->type == ix::WebSocketMessageType::Close) {
						connected = false;
						log("Connection closed: " + msg->closeInfo.reason, LogLevel::INFO);

						// Notify connection waiters in case close happens during connection attempt
						{
							std::lock_guard lock(connectionMutex);
							connectionAttemptComplete = true;
						}
						connectionCV.notify_all();

						if (!isShutdown) {
							updateStatus(StreamStatus::DISCONNECTED);
							// Attempt reconnection if enabled (Issue #6 fix: managed thread)
							if (autoReconnect && !reconnectionInProgress) {
								startReconnectionThread();
							}
						}
					}
					else if (msg->type == ix::WebSocketMessageType::Error) {
						log("WebSocket error: " + msg->errorInfo.reason, LogLevel::ERR);
						connected = false;

						// Notify connection waiters
						{
							std::lock_guard lock(connectionMutex);
							connectionAttemptComplete = true;
						}
						connectionCV.notify_all();

						if (!isShutdown) {
							updateStatus(StreamStatus::DISCONNECTED);
						}
					}
					else if (msg->type == ix::WebSocketMessageType::Ping) {
						log("Received ping", LogLevel::DEBUG);
					}
					else if (msg->type == ix::WebSocketMessageType::Pong) {
						log("Received pong", LogLevel::DEBUG);
					}
				});

				// Start the connection
				webSocket.start();

				std::unique_lock lock(connectionMutex);
				if (!connectionCV.wait_for(lock, timeout, [this]() {
					return connectionAttemptComplete || isShutdown;
				})) {
					// Timeout occurred
					webSocket.stop();
					throw std::runtime_error("Connection timeout");
				}

				if (isShutdown) {
					webSocket.stop();
					throw std::runtime_error("Connection aborted due to shutdown");
				}

				// Check if actually connected (could have received error or close)
				if (!connected) {
					throw std::runtime_error("Connection failed");
				}
			}
			catch (const std::exception& e) {
				log("Connection failed: " + std::string(e.what()), LogLevel::ERR);
				connected = false;
				updateStatus(StreamStatus::DISCONNECTED);
				throw;
			}
			catch (...) {
				log("Connection failed: unknown error", LogLevel::ERR);
				connected = false;
				updateStatus(StreamStatus::DISCONNECTED);
				throw;
			}
		}

		/// <summary>
		/// Internal disconnect implementation (not thread-safe, must be called with wsMutex held).
		/// </summary>
		void disconnectInternal() {
			if (connected || webSocket.getReadyState() != ix::ReadyState::Closed) {
				try {
					// Stop the WebSocket - this is synchronous and waits for the internal
					// thread to finish, but callbacks may still be queued
					webSocket.stop();

					// Brief wait to ensure all queued callbacks complete.
					// IXWebSocket's stop() is synchronous, but OS-level socket operations
					// and callback dispatch may still be in flight on some platforms.
					std::this_thread::sleep_for(std::chrono::milliseconds(50));

					log("Disconnected", LogLevel::INFO);
				}
				catch (const std::exception& e) {
					log("Error during disconnect: " + std::string(e.what()), LogLevel::WARNING);
				}
				catch (...) {
					log("Unknown error during disconnect", LogLevel::WARNING);
				}
				connected = false;
				if (!isShutdown) {
					updateStatus(StreamStatus::DISCONNECTED);
				}
			}
		}

		/// <summary>
		/// Starts a managed reconnection thread.
		/// </summary>
		void startReconnectionThread() {
			// Clean up any previous reconnection thread
			{
				std::lock_guard lock(reconnectMutex);
				if (reconnectThread && reconnectThread->joinable()) {
					// Previous thread is still running, don't start another
					return;
				}
			}

			reconnectionInProgress = true;

			std::lock_guard lock(reconnectMutex);
			reconnectThread = std::make_unique<std::thread>([this]() {
				attemptReconnection();
				reconnectionInProgress = false;
			});
		}

		/// <summary>
		/// Stops and joins the reconnection thread if active.
		/// </summary>
		void stopReconnectionThread() {
			std::unique_ptr<std::thread> threadToJoin;

			{
				std::lock_guard lock(reconnectMutex);
				if (reconnectThread) {
					threadToJoin = std::move(reconnectThread);
				}
			}

			// Join outside the lock to avoid deadlock
			if (threadToJoin && threadToJoin->joinable()) {
				threadToJoin->join();
			}
		}

		/// <summary>
		/// Attempts to reconnect with exponential backoff.
		/// </summary>
		void attemptReconnection() {
			if (!autoReconnect || isShutdown) {
				return;
			}

			log("Attempting automatic reconnection", LogLevel::INFO);

			int maxAttempts;
			{
				std::lock_guard lock(configMutex);
				maxAttempts = maxReconnectAttempts;
			}

			for (int attempt = 0; attempt < maxAttempts; ++attempt) {
				if (isShutdown || !autoReconnect) {
					break;
				}

				// Exponential backoff: 1s, 2s, 4s, etc.
				auto delay = std::chrono::seconds(1 << attempt);
				log("Reconnection attempt " + std::to_string(attempt + 1) + " of " +
					std::to_string(maxAttempts) + " in " +
					std::to_string(delay.count()) + " seconds", LogLevel::INFO);

				std::this_thread::sleep_for(delay);

				if (isShutdown || !autoReconnect) {
					break;
				}

				try {
					std::lock_guard lock(wsMutex);
					if (!isShutdown && autoReconnect) {
						connectInternal();
						log("Reconnection successful", LogLevel::INFO);
						return;
					}
				}
				catch (const std::exception& e) {
					log("Reconnection attempt failed: " + std::string(e.what()), LogLevel::WARNING);
				}
				catch (...) {
					log("Reconnection attempt failed: unknown error", LogLevel::WARNING);
				}
			}

			log("Automatic reconnection failed after " + std::to_string(maxAttempts) + " attempts", LogLevel::ERR);
		}

		/// <summary>
		/// Reconnects to the data stream if already connected.
		/// Used when the request format, replay speed, or time period is changed to update the connection.
		/// </summary>
		void reconnectIfConnected() {
			if (isShutdown) return;

			std::lock_guard lock(wsMutex);
			if (connected && !isShutdown && !reconnectionInProgress) {
				log("Reconnecting due to configuration change", LogLevel::INFO);
				connectInternal();
			}
		}

		/// <summary>
		/// Notifies all status listeners of a status change.
		/// </summary>
		/// <param name="newStatus">The new status.</param>
		void updateStatus(StreamStatus newStatus) {
			std::vector<StatusListener> listenersCopy;

			{
				std::lock_guard lock(listenerMutex);
				listenersCopy.reserve(statusListeners.size());
				for (const auto& [id, listener] : statusListeners) {
					listenersCopy.push_back(listener);
				}
			}

			// Call listeners outside the lock to avoid deadlocks
			for (const auto& listener : listenersCopy) {
				try {
					listener(newStatus);
				}
				catch (const std::exception& e) {
					log("Error in status listener callback: " + std::string(e.what()), LogLevel::ERR);
				}
				catch (...) {
					log("Unknown error in status listener callback", LogLevel::ERR);
				}
			}
		}

	public:
		/// <summary>
		/// Creates a new data stream listener for the specified data stream.
		/// By default, the listener will listen to the data stream in real-time.
		/// To listen to historical data, use the setTimeExtent method.
		/// </summary>
		/// <param name="dataStream">The data stream to listen to.</param>
		explicit StreamListener(const OSHDataStream & dataStream) : dataStream(dataStream) {}

		virtual ~StreamListener() {
			shutdown();
			stopReconnectionThread();
		}

		StreamListener(const StreamListener&) = delete;
		StreamListener& operator=(const StreamListener&) = delete;
		StreamListener(StreamListener&&) = delete;
		StreamListener& operator=(StreamListener&&) = delete;

		/// <summary>
		/// Called when the data stream receives an update.
		/// Parses the data and calls the onStreamUpdate(StreamEventArgs) overload.
		/// </summary>
		/// <param name="data">The raw data received from the data stream.</param>
		void onStreamUpdate(const std::vector<uint8_t>&data) {
			Constants::RequestFormat format = determineRequestFormat(data);
			int64_t timestamp = determineTimestamp(format, data);
			onStreamUpdate(StreamEventArgs(timestamp, data, format, &dataStream));
		}

		/// <summary>
		/// Called when the data stream receives an update.
		/// Override this method in derived classes to handle the incoming data as needed.
		/// This method may be called from the listen() thread.
		/// </summary>
		/// <param name="args">The event arguments containing the data and metadata.</param>
		virtual void onStreamUpdate(const StreamEventArgs & args) {
			// Default implementation does nothing - override in derived classes
		}

		/// <summary>
		/// Connects to the data stream using the specified request format, replay speed, and time period.
		/// This method blocks until the connection is established or times out.
		/// Once connected, ixwebsocket handles all message processing automatically via callbacks.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		void connect() {
			std::lock_guard lock(wsMutex);
			connectInternal();
		}

		/// <summary>
		/// Disconnects from the data stream.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		void disconnect() {
			// Stop any reconnection attempts
			autoReconnect = false;
			stopReconnectionThread();

			std::lock_guard lock(wsMutex);
			disconnectInternal();
		}

		/// <summary>
		/// Shuts down the data stream listener.
		/// Disconnects from the data stream and prevents further connections.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		void shutdown() {
			isShutdown = true;
			autoReconnect = false;
			stopReconnectionThread();
			disconnect();
			updateStatus(StreamStatus::SHUTDOWN);
		}

		/// <summary>
		/// Checks if the connection is active and the socket is open.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <returns>True if connected and socket is open, false otherwise.</returns>
		bool isConnected() const {
			if (isShutdown) {
				return false;
			}
			std::lock_guard lock(wsMutex);
			return connected && webSocket.getReadyState() == ix::ReadyState::Open;
		}

		/// <summary>
		/// Gets the current connection status.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <returns>The current StreamStatus.</returns>
		StreamStatus getStatus() const {
			if (isShutdown) {
				return StreamStatus::SHUTDOWN;
			}
			else if (connected) {
				return StreamStatus::CONNECTED;
			}
			else {
				return StreamStatus::DISCONNECTED;
			}
		}

		/// <summary>
		/// Adds a status listener to be notified of connection state changes.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="listener">The listener callback function.</param>
		/// <returns>A unique ID for this listener, used for removal.</returns>
		ListenerId addStatusListener(StatusListener const& listener) {
			std::lock_guard lock(listenerMutex);
			statusListeners[nextListenerId] = listener;
			return nextListenerId++;
		}

		/// <summary>
		/// Removes a status listener by ID.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="id">The ID returned by addStatusListener.</param>
		void removeStatusListener(ListenerId id) {
			std::lock_guard lock(listenerMutex);
			statusListeners.erase(id);
		}

		/// <summary>
		/// Sets a custom logger callback for logging messages.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="callback">The logger callback function.</param>
		void setLogger(LogCallback callback) {
			std::lock_guard lock(loggerMutex);
			logger = callback;
		}

		/// <summary>
		/// Removes the custom logger callback.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		void clearLogger() {
			std::lock_guard lock(loggerMutex);
			logger.reset();
		}

		/// <summary>
		/// Enables or disables automatic reconnection on connection loss.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="enable">True to enable auto-reconnect, false to disable.</param>
		void enableAutoReconnect(bool enable) {
			autoReconnect = enable;
		}

		/// <summary>
		/// Gets the auto-reconnect setting.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <returns>True if auto-reconnect is enabled, false otherwise.</returns>
		bool isAutoReconnectEnabled() const {
			return autoReconnect;
		}

		/// <summary>
		/// Sets the maximum number of reconnection attempts.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="attempts">Maximum number of reconnection attempts (must be > 0).</param>
		void setMaxReconnectAttempts(int attempts) {
			if (attempts <= 0) {
				throw std::invalid_argument("Max reconnect attempts must be greater than 0");
			}
			std::lock_guard lock(configMutex);
			maxReconnectAttempts = attempts;
		}

		/// <summary>
		/// Gets the maximum number of reconnection attempts.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <returns>The maximum number of reconnection attempts.</returns>
		int getMaxReconnectAttempts() const {
			std::lock_guard lock(configMutex);
			return maxReconnectAttempts;
		}

		/// <summary>
		/// Sets the connection timeout duration.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="timeout">Timeout duration in seconds.</param>
		void setConnectionTimeout(std::chrono::seconds timeout) {
			std::lock_guard lock(configMutex);
			connectionTimeout = timeout;
		}

		/// <summary>
		/// Gets the connection timeout duration.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <returns>The connection timeout duration in seconds.</returns>
		std::chrono::seconds getConnectionTimeout() const {
			std::lock_guard lock(configMutex);
			return connectionTimeout;
		}

		/// <summary>
		/// The format of the request.
		/// If nullopt, the format will not be specified in the request, i.e., the data will be received in the default format.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		std::optional<Constants::RequestFormat> getRequestFormat() const {
			std::lock_guard lock(configMutex);
			return requestFormat;
		}

		/// <summary>
		/// Sets the format of the request.
		/// If nullopt, the format will not be specified in the request, i.e., the data will be received in the default format.
		/// Calling this method will reconnect to the data stream if it is already connected.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="format">The format of the request.</param>
		void setRequestFormat(Constants::RequestFormat format) {
			{
				std::lock_guard lock(configMutex);
				requestFormat = format;
			}
			reconnectIfConnected();
		}

		/// <summary>
		/// The time period for the data stream.
		/// If nullopt, the time period will not be specified in the request, i.e., will listen to the data stream in real-time.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		std::optional<ConnectedSystemsAPI::DataModels::TimeExtent> getTimeExtent() const {
			std::lock_guard lock(configMutex);
			return timeExtent;
		}

		/// <summary>
		/// Sets the time period for the data stream.
		/// If nullopt, the time period will not be specified in the request, i.e., will listen to the data stream in real-time.
		/// Calling this method will reconnect to the data stream if it is already connected.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="extent">The time period of the request. Use nullopt to remove the previously set time period.</param>
		void setTimeExtent(const ConnectedSystemsAPI::DataModels::TimeExtent & extent) {
			{
				std::lock_guard lock(configMutex);
				timeExtent = extent;
			}
			reconnectIfConnected();
		}

		/// <summary>
		/// The replay speed for the data stream.
		/// Only applicable for historical data streams.
		/// 1.0 is the default speed, 0.1 is 10 times slower, 10.0 is 10 times faster.
		/// Zero or negative values will result in no data being received.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		double getReplaySpeed() const {
			std::lock_guard lock(configMutex);
			return replaySpeed;
		}

		/// <summary>
		/// Sets the replay speed for the data stream.
		/// Only applicable for historical data streams.
		/// 1.0 is the default speed, 0.1 is 10 times slower, 10.0 is 10 times faster.
		/// Zero or negative values will result in no data being received.
		/// Calling this method will reconnect to the data stream if it is already connected.
		/// Thread-safe: can be called from multiple threads.
		/// </summary>
		/// <param name="speed">The replay speed of the request.</param>
		void setReplaySpeed(double speed) {
			{
				std::lock_guard lock(configMutex);
				replaySpeed = speed;
			}
			reconnectIfConnected();
		}

		/// <summary>
		/// The data stream being listened to.
		/// </summary>
		OSHDataStream& getDataStream() { return dataStream; }

		/// <summary>
		/// The data stream being listened to.
		/// </summary>
		const OSHDataStream& getDataStream() const { return dataStream; }
	};
}