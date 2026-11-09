#include "pch.h"
#include "CppUnitTest.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "TestHelper.h"
#include "../OSHConnect/OSHConnect.h"
#include "../OSHConnect/OSHNode.h"
#include "../OSHConnect/OSHSystem.h"
#include "../OSHConnect/OSHDataStream.h"
#include "../OSHConnect/Websocket/StreamHandler.h"
#include "../OSHConnect/Websocket/StreamListener.h"

#include <CSAPI/DataModels/DataStreamBuilder.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::string_literals;
using OSHConnect::Websocket::StreamStatus;

namespace OSHConnectTest
{
	/// <summary>
	/// Tests that callbacks can call back into OSHConnect (connect, disconnect, remove listeners, shut down)
	/// without deadlocking. Every wait has a timeout, so a deadlock fails the test instead of hanging it.
	/// </summary>
	TEST_CLASS(CallbackReentryTest)
	{
		/// <summary>
		/// A local websocket server that sends a burst of observations on every connection as soon as it opens.
		/// </summary>
		struct BurstServer {
			std::unique_ptr<ix::WebSocketServer> server;
			int port = 0;

			BurstServer() {
				ix::initNetSystem();
				for (int candidate = 19365; candidate < 19465 && !server; ++candidate) {
					auto attempt = std::make_unique<ix::WebSocketServer>(candidate, "127.0.0.1");
					if (attempt->listen().first) {
						server = std::move(attempt);
						port = candidate;
					}
				}
				Assert::IsNotNull(server.get(), L"No free port for the local websocket server");
				server->setOnClientMessageCallback([](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& webSocket, const ix::WebSocketMessagePtr& msg) {
					if (msg->type != ix::WebSocketMessageType::Open) return;
					for (int i = 0; i < 50; ++i) {
						webSocket.sendText(R"({"datastream@id":"ds","phenomenonTime":"2026-01-02T03:04:05.678Z","result":{}})");
					}
				});
				server->start();
			}

			~BurstServer() { server->stop(); }

			std::string root() const { return "http://127.0.0.1:" + std::to_string(port) + "/sensorhub"; }
		};

		/// <summary>
		/// A data stream on a node at the given root, built without discovery.
		/// </summary>
		struct TestStreams {
			TestHelper helper;
			std::shared_ptr<OSHConnect::OSHNode> node;
			std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> api;
			std::shared_ptr<OSHConnect::OSHSystem> system;

			explicit TestStreams(const std::string& root)
				: node(std::make_shared<OSHConnect::OSHNode>(root, "token", false)),
				api(node, &node->getConSysAPI()),
				system(std::make_shared<OSHConnect::OSHSystem>(node, api, helper.createTestSystemObject())) {
			}

			std::shared_ptr<OSHConnect::OSHDataStream> make(const std::string& id) {
				return std::make_shared<OSHConnect::OSHDataStream>(system, api,
					ConnectedSystemsAPI::DataModels::DataStreamBuilder(helper.createTestDataStreamObject()).withId(id).build());
			}
		};

		/// <summary>
		/// Waits up to 10 seconds for a condition that other threads signal with notify().
		/// The condition is checked outside the lock, since it may call into OSHConnect.
		/// </summary>
		struct Waiter {
			std::mutex mutex;
			std::condition_variable condition;

			void notify() {
				{ std::lock_guard lock(mutex); }
				condition.notify_all();
			}

			bool waitFor(const std::function<bool()>& done) {
				auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (!done()) {
					if (std::chrono::steady_clock::now() >= deadline) return false;
					std::unique_lock lock(mutex);
					condition.wait_for(lock, std::chrono::milliseconds(20));
				}
				return true;
			}
		};

		/// <summary>
		/// A listener that runs an action from inside its first onStreamUpdate.
		/// </summary>
		class ActingListener : public OSHConnect::Websocket::StreamListener {
		public:
			std::function<void(ActingListener&)> onFirstUpdate;
			std::atomic<int> updates{ 0 };
			std::atomic<bool> actionReturned{ false };
			Waiter* waiter = nullptr;

			using StreamListener::StreamListener;

			~ActingListener() override { shutdown(); }

			void onStreamUpdate(const OSHConnect::Websocket::StreamEventArgs&) override {
				if (++updates == 1 && onFirstUpdate) {
					onFirstUpdate(*this);
					actionReturned = true;
				}
				if (waiter) waiter->notify();
			}
		};

	public:
		TEST_METHOD(HandlerCallback_RemovesAnotherStream) {
			BurstServer server;
			TestStreams streams(server.root());
			auto streamA = streams.make("dsA");
			auto streamB = streams.make("dsB");

			Waiter waiter;
			std::atomic<bool> removed{ false };
			std::atomic<bool> returned{ false };
			OSHConnect::OSHConnect oshConnect;
			OSHConnect::Websocket::StreamHandler* handler = nullptr;
			handler = oshConnect.getStreamManager().createDataStreamHandler([&](const OSHConnect::Websocket::StreamEventArgs& args) {
				if (!returned && args.getDataStream() == streamA) {
					// Stopping B waits for B's network thread, which is busy delivering B's own burst
					removed = handler->shutdownDataStreamListener(*streamB);
					returned = true;
					waiter.notify();
				}
			});
			handler->addDataStreamListener(streamA);
			handler->addDataStreamListener(streamB);
			handler->connect();

			Assert::IsTrue(waiter.waitFor([&] { return returned.load(); }), L"Removing another stream from a callback deadlocked");
			Assert::IsTrue(removed.load());
			Assert::AreEqual(size_t(1), handler->getDataStreamListenerCount());
			oshConnect.getStreamManager().shutdownDataStreamHandler(handler);
		}

		TEST_METHOD(HandlerCallback_ShutsDownTheHandler) {
			BurstServer server;
			TestStreams streams(server.root());

			Waiter waiter;
			std::atomic<int> updates{ 0 };
			std::atomic<bool> returned{ false };
			OSHConnect::OSHConnect oshConnect;
			OSHConnect::Websocket::StreamHandler* handler = nullptr;
			handler = oshConnect.getStreamManager().createDataStreamHandler([&](const OSHConnect::Websocket::StreamEventArgs&) {
				if (++updates == 1) {
					handler->shutdown();
					returned = true;
					waiter.notify();
				}
			});
			handler->addDataStreamListener(streams.make("dsA"));
			handler->addDataStreamListener(streams.make("dsB"));
			handler->connect();

			Assert::IsTrue(waiter.waitFor([&] { return returned.load(); }), L"Shutting down the handler from its callback deadlocked");
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			Assert::AreEqual(1, updates.load(), L"Updates were delivered after shutdown");
			Assert::IsTrue(handler->getStatus() == StreamStatus::SHUTDOWN);
			Assert::AreEqual(size_t(0), handler->getDataStreamListenerCount());
		}

		TEST_METHOD(HandlerCallback_ManagerDestroysTheHandler) {
			BurstServer server;
			TestStreams streams(server.root());

			// Released when the handler (which holds the callback) is destroyed
			struct Sentinel {
				std::atomic<bool>* destroyed;
				Waiter* waiter;
				Sentinel(std::atomic<bool>* destroyed, Waiter* waiter) : destroyed(destroyed), waiter(waiter) {}
				~Sentinel() { *destroyed = true; waiter->notify(); }
			};
			std::atomic<bool> destroyed{ false };
			Waiter waiter;
			auto sentinel = std::make_shared<Sentinel>(&destroyed, &waiter);

			OSHConnect::OSHConnect oshConnect;
			auto& streamManager = oshConnect.getStreamManager();
			OSHConnect::Websocket::StreamHandler* handler = nullptr;
			std::atomic<bool> called{ false };
			handler = streamManager.createDataStreamHandler([&, sentinel](const OSHConnect::Websocket::StreamEventArgs&) {
				if (!called.exchange(true)) {
					streamManager.shutdownDataStreamHandler(handler);
				}
			});
			sentinel.reset();
			handler->addDataStreamListener(streams.make("dsA"));
			handler->connect();

			Assert::IsTrue(waiter.waitFor([&] { return destroyed.load(); }), L"The handler was not destroyed after its callback returned");
			Assert::IsTrue(streamManager.getDataStreamHandlers().empty());
		}

		TEST_METHOD(ListenerCallback_DisconnectsItself) {
			BurstServer server;
			TestStreams streams(server.root());

			Waiter waiter;
			ActingListener listener(streams.make("dsA"));
			listener.waiter = &waiter;
			// MSVC reports a thread joining itself as an error instead of hanging, and the listener logs it
			std::mutex problemsMutex;
			std::string problems;
			listener.setLogger([&](const std::string& message, OSHConnect::Websocket::LogLevel level) {
				if (level == OSHConnect::Websocket::LogLevel::WARNING || level == OSHConnect::Websocket::LogLevel::ERR) {
					std::lock_guard lock(problemsMutex);
					problems += message + "\n";
				}
			});
			listener.onFirstUpdate = [](ActingListener& self) { self.disconnect(); };
			listener.connect();

			Assert::IsTrue(waiter.waitFor([&] { return listener.actionReturned.load(); }), L"disconnect() from the listener's own callback did not return");
			Assert::IsTrue(waiter.waitFor([&] { return !listener.isConnected(); }), L"The deferred disconnect never ran");
			{
				std::lock_guard lock(problemsMutex);
				Logger::WriteMessage(problems.c_str());
				Assert::IsTrue(problems.empty(), L"The disconnect reported an error");
			}

			// The listener still works afterwards
			listener.connect();
			Assert::IsTrue(listener.isConnected());
		}

		TEST_METHOD(ListenerCallback_ReconnectsItself) {
			BurstServer server;
			TestStreams streams(server.root());

			Waiter waiter;
			ActingListener listener(streams.make("dsA"));
			listener.waiter = &waiter;
			// Changing a setting reconnects; from a callback, that has to wait until the callback returns
			listener.onFirstUpdate = [](ActingListener& self) { self.setReplaySpeed(2.0); };
			listener.connect();

			Assert::IsTrue(waiter.waitFor([&] { return listener.actionReturned.load(); }), L"Reconnecting from the listener's own callback did not return");
			// The reconnection opens a second connection, which sends a second burst
			Assert::IsTrue(waiter.waitFor([&] { return listener.updates.load() > 50; }), L"The deferred reconnect never ran");
			Assert::AreEqual(2.0, listener.getReplaySpeed());
		}

		TEST_METHOD(StatusCallback_DuringFailedConnect_CanDisconnect) {
			// A failed connect reports DISCONNECTED on the calling thread while holding the listener's lock,
			// so calling disconnect() from that status callback used to lock the same mutex twice
			TestStreams streams("http://127.0.0.1:1/sensorhub");
			ActingListener listener(streams.make("dsA"));
			listener.setConnectionTimeout(std::chrono::seconds(5));

			// Act on the DISCONNECTED reported on the connecting thread, which still holds the lock;
			// the network thread reports one too, without the lock
			std::atomic<std::thread::id> connectingThread{};
			std::atomic<bool> statusCallbackReturned{ false };
			std::atomic<bool> firstCall{ true };
			std::atomic<bool> threw{ false };
			listener.addStatusListener([&](StreamStatus status) {
				if (status == StreamStatus::DISCONNECTED && std::this_thread::get_id() == connectingThread.load()
					&& firstCall.exchange(false)) {
					// MSVC reports a mutex locked twice by one thread as an exception instead of hanging
					try {
						listener.disconnect();
					}
					catch (...) {
						threw = true;
					}
					statusCallbackReturned = true;
				}
			});

			auto connecting = std::thread([&] {
				connectingThread = std::this_thread::get_id();
				try { listener.connect(); }
				catch (const std::exception&) {}
			});
			auto start = std::chrono::steady_clock::now();
			while (!statusCallbackReturned && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			const bool returned = statusCallbackReturned.load();
			if (returned) {
				connecting.join();
			}
			else {
				connecting.detach();
			}
			Assert::IsTrue(returned, L"disconnect() from a status callback during connect deadlocked");
			Assert::IsFalse(threw.load(), L"disconnect() from a status callback during connect failed");
		}
	};
}
