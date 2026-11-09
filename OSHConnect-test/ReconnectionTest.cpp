#include "pch.h"
#include "CppUnitTest.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "TestHelper.h"
#include "../OSHConnect/OSHNode.h"
#include "../OSHConnect/OSHSystem.h"
#include "../OSHConnect/OSHDataStream.h"
#include "../OSHConnect/Websocket/StreamListener.h"

#include <CSAPI/DataModels/DataStreamBuilder.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace OSHConnectTest
{
	/// <summary>
	/// Tests for StreamListener's automatic reconnection.
	/// A local websocket server counts connections and can drop them, as a node going away would.
	/// </summary>
	TEST_CLASS(ReconnectionTest)
	{
		/// <summary>
		/// Finds a local port that nothing is listening on.
		/// </summary>
		static int freePort(int from) {
			for (int candidate = from; candidate < from + 100; ++candidate) {
				ix::WebSocketServer probe(candidate, "127.0.0.1");
				if (probe.listen().first) {
					probe.stop();
					return candidate;
				}
			}
			Assert::Fail(L"No free local port");
			return 0;
		}

		/// <summary>
		/// A local websocket server that counts the connections it accepts.
		/// </summary>
		struct CountingServer {
			std::unique_ptr<ix::WebSocketServer> server;
			std::atomic<int> connections{ 0 };

			explicit CountingServer(int port) {
				server = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1");
				Assert::IsTrue(server->listen().first, L"Could not listen on the test port");
				server->setOnClientMessageCallback([this](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&, const ix::WebSocketMessagePtr& msg) {
					if (msg->type == ix::WebSocketMessageType::Open) ++connections;
				});
				server->start();
			}

			~CountingServer() { server->stop(); }

			/// <summary>
			/// Closes every client connection from the server's side.
			/// </summary>
			void dropClients() {
				for (auto& client : server->getClients()) {
					client->close();
				}
			}

			size_t openClients() { return server->getClients().size(); }
		};

		static std::shared_ptr<OSHConnect::OSHDataStream> makeStream(int port) {
			TestHelper helper;
			auto node = std::make_shared<OSHConnect::OSHNode>("http://127.0.0.1:" + std::to_string(port) + "/sensorhub", "token", false);
			std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> api(node, &node->getConSysAPI());
			auto system = std::make_shared<OSHConnect::OSHSystem>(node, api, helper.createTestSystemObject());
			// The data stream shares ownership of the API, which keeps the node's ConSysAPI alive
			return std::make_shared<OSHConnect::OSHDataStream>(system, api,
				ConnectedSystemsAPI::DataModels::DataStreamBuilder(helper.createTestDataStreamObject()).withId("ds1").build());
		}

		static bool waitUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
			auto deadline = std::chrono::steady_clock::now() + timeout;
			while (!done()) {
				if (std::chrono::steady_clock::now() >= deadline) return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			return true;
		}

	public:
		TEST_METHOD_INITIALIZE(Initialize) {
			ix::initNetSystem();
		}

		TEST_METHOD(AutoReconnect_RestoresTheConnectionEveryTime) {
			const int port = freePort(19565);
			CountingServer server(port);
			OSHConnect::Websocket::StreamListener listener(makeStream(port));
			listener.enableAutoReconnect(true);
			listener.connect();
			Assert::IsTrue(waitUntil([&] { return server.connections == 1; }));

			// The first reconnection used to be the only one: later drops were never restored
			for (int drop = 1; drop <= 3; ++drop) {
				server.dropClients();
				Assert::IsTrue(waitUntil([&] { return server.connections == drop + 1 && listener.isConnected(); }),
					(L"Connection not restored after drop " + std::to_wstring(drop)).c_str());
			}

			// Only the listener's own reconnection runs, so there is never more than one connection
			Assert::AreEqual(size_t(1), server.openClients());
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
			Assert::AreEqual(4, server.connections.load(), L"Extra connections were made");
		}

		TEST_METHOD(Disconnect_KeepsAutoReconnect_AndDoesNotReconnect) {
			const int port = freePort(19665);
			CountingServer server(port);
			OSHConnect::Websocket::StreamListener listener(makeStream(port));
			listener.enableAutoReconnect(true);
			listener.connect();
			Assert::IsTrue(waitUntil([&] { return server.connections == 1; }));

			listener.disconnect();
			Assert::IsTrue(listener.isAutoReconnectEnabled(), L"disconnect() turned auto-reconnect off");

			// A manual disconnect is not a dropped connection
			std::this_thread::sleep_for(std::chrono::milliseconds(2500));
			Assert::AreEqual(1, server.connections.load(), L"The listener reconnected after a manual disconnect");

			// After connecting again, auto-reconnect still works
			listener.connect();
			Assert::IsTrue(waitUntil([&] { return server.connections == 2; }));
			server.dropClients();
			Assert::IsTrue(waitUntil([&] { return server.connections == 3 && listener.isConnected(); }),
				L"Auto-reconnect did not work after disconnect and connect");
		}

		TEST_METHOD(FailedConnect_DoesNotKeepRetrying) {
			const int port = freePort(19765);
			OSHConnect::Websocket::StreamListener listener(makeStream(port));
			listener.setConnectionTimeout(std::chrono::seconds(5));

			bool threw = false;
			try {
				listener.connect();
			}
			catch (const std::exception&) {
				threw = true;
			}
			Assert::IsTrue(threw, L"connect() should fail with nothing listening");

			// ixwebsocket's own reconnection used to keep retrying, and connect once a server appeared
			CountingServer server(port);
			std::this_thread::sleep_for(std::chrono::seconds(4));
			Assert::AreEqual(0, server.connections.load(), L"The listener kept retrying after connect() failed");
			Assert::IsFalse(listener.isConnected());
		}

		TEST_METHOD(SettingChange_ReconnectsOnce) {
			const int port = freePort(19865);
			CountingServer server(port);
			OSHConnect::Websocket::StreamListener listener(makeStream(port));
			listener.enableAutoReconnect(true);
			listener.connect();
			Assert::IsTrue(waitUntil([&] { return server.connections == 1; }));

			listener.setReplaySpeed(2.0);
			Assert::IsTrue(waitUntil([&] { return server.connections == 2; }));

			// The close caused by the change used to also start an automatic reconnection, a second later
			std::this_thread::sleep_for(std::chrono::milliseconds(2500));
			Assert::AreEqual(2, server.connections.load(), L"A setting change reconnected more than once");
			Assert::IsTrue(listener.isConnected());
		}

		TEST_METHOD(Disconnect_DuringBackoff_ReturnsQuickly) {
			const int port = freePort(19965);
			auto server = std::make_unique<CountingServer>(port);
			OSHConnect::Websocket::StreamListener listener(makeStream(port));
			listener.enableAutoReconnect(true);
			listener.setMaxReconnectAttempts(5);
			listener.connect();
			Assert::IsTrue(waitUntil([&] { return server->connections == 1; }));

			// The node goes away: reconnection attempts fail, and the listener backs off 1 s, 2 s, 4 s...
			server.reset();
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));

			auto start = std::chrono::steady_clock::now();
			listener.disconnect();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

			Logger::WriteMessage(("disconnect() took " + std::to_string(elapsed.count()) + " ms\n").c_str());
			Assert::IsTrue(elapsed < std::chrono::milliseconds(1000), L"disconnect() waited for the reconnection backoff");
		}
	};
}
