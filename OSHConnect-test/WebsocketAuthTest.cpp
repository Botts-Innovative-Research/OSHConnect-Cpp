#include "pch.h"
#include "CppUnitTest.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "TestHelper.h"
#include "../OSHConnect/OSHConnect.h"
#include "../OSHConnect/OSHNode.h"
#include "../OSHConnect/OSHSystem.h"
#include "../OSHConnect/OSHDataStream.h"
#include "../OSHConnect/Websocket/StreamHandler.h"

#include <CSAPI/DataModels/DataStreamBuilder.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::string_literals;

namespace OSHConnectTest
{
	TEST_CLASS(WebsocketAuthTest)
	{
		struct Handshake {
			std::string uri;
			std::optional<std::string> authorization;
		};

		/// <summary>
		/// Connects a data stream listener to a local websocket server and returns the handshake the server received.
		/// </summary>
		template<typename AddNode>
		static Handshake captureHandshake(AddNode addNode) {
			ix::initNetSystem();

			// Find a free local port
			std::unique_ptr<ix::WebSocketServer> server;
			int port = 0;
			for (int candidate = 18765; candidate < 18865 && !server; ++candidate) {
				auto attempt = std::make_unique<ix::WebSocketServer>(candidate, "127.0.0.1");
				if (attempt->listen().first) {
					server = std::move(attempt);
					port = candidate;
				}
			}
			Assert::IsNotNull(server.get(), L"No free port for the local websocket server");

			std::mutex mutex;
			std::condition_variable opened;
			std::optional<Handshake> handshake;
			server->setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&, const ix::WebSocketMessagePtr& msg) {
				if (msg->type != ix::WebSocketMessageType::Open) return;
				std::lock_guard lock(mutex);
				Handshake h{ msg->openInfo.uri, std::nullopt };
				auto it = msg->openInfo.headers.find("Authorization");
				if (it != msg->openInfo.headers.end()) h.authorization = it->second;
				handshake = h;
				opened.notify_all();
			});
			server->start();

			TestHelper helper;
			OSHConnect::OSHConnect oshConnect("TestOSHConnect");
			auto node = addNode(oshConnect, "http://127.0.0.1:" + std::to_string(port) + "/sensorhub");
			OSHConnect::OSHSystem system(&node, helper.createTestSystemObject());
			OSHConnect::OSHDataStream dataStream(&system,
				ConnectedSystemsAPI::DataModels::DataStreamBuilder(helper.createTestDataStreamObject()).withId("ds1").build());

			auto& streamManager = oshConnect.getStreamManager();
			auto* handler = streamManager.createDataStreamHandler([](const OSHConnect::Websocket::StreamEventArgs&) {});
			handler->addDataStreamListener(dataStream);
			handler->connect();

			{
				std::unique_lock lock(mutex);
				opened.wait_for(lock, std::chrono::seconds(10), [&] { return handshake.has_value(); });
			}

			streamManager.shutdownDataStreamHandler(handler);
			server->stop();

			Assert::IsTrue(handshake.has_value(), L"The local websocket server never received a connection");
			return handshake.value();
		}

	public:
		TEST_METHOD(Connect_BasicAuth_SendsAuthorizationHeader) {
			auto handshake = captureHandshake([](OSHConnect::OSHConnect& osh, const std::string& root) {
				return osh.getNodeManager().addNode(root, "admin", "password");
			});

			Assert::AreEqual("/sensorhub/api/datastreams/ds1/observations"s, handshake.uri);
			Assert::IsTrue(handshake.authorization.has_value(), L"No Authorization header was sent");
			Assert::AreEqual("Basic YWRtaW46cGFzc3dvcmQ="s, handshake.authorization.value());
		}

		TEST_METHOD(Connect_BearerToken_SendsAuthorizationHeader) {
			auto handshake = captureHandshake([](OSHConnect::OSHConnect& osh, const std::string& root) {
				return osh.getNodeManager().addNode(root, "mytoken123"s, false);
			});

			Assert::AreEqual("Bearer mytoken123"s, handshake.authorization.value_or(""));
		}

		TEST_METHOD(Connect_NoToken_SendsNoAuthorizationHeader) {
			auto handshake = captureHandshake([](OSHConnect::OSHConnect& osh, const std::string& root) {
				return osh.getNodeManager().addNode(root, ""s, false);
			});

			Assert::IsFalse(handshake.authorization.has_value());
		}
	};
}
