#include "pch.h"
#include "CppUnitTest.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <httplib.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "LocalNodeServer.h"
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
	// Copying these objects is what used to leave children pointing at destroyed parents
	static_assert(!std::is_copy_constructible_v<OSHConnect::OSHNode>, "OSHNode must not be copyable");
	static_assert(!std::is_copy_constructible_v<OSHConnect::OSHSystem>, "OSHSystem must not be copyable");
	static_assert(!std::is_copy_constructible_v<OSHConnect::OSHDataStream>, "OSHDataStream must not be copyable");
	static_assert(!std::is_copy_constructible_v<OSHConnect::OSHControlStream>, "OSHControlStream must not be copyable");

	/// <summary>
	/// Tests that nodes, systems, and data streams are shared objects with safe parent links.
	/// A LocalNodeServer stands in for an OpenSensorHub node with one system and one data stream.
	/// </summary>
	TEST_CLASS(ObjectLifetimeTest)
	{
	public:
		TEST_METHOD(Discovery_ReturnsTheNodesOwnObjects) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto node = oshConnect.getNodeManager().addNode(local.root(), "admin", "admin");

			auto first = node->discoverSystems();
			auto second = node->discoverSystems();
			Assert::AreEqual(size_t(1), first.size());
			Assert::IsTrue(first[0] == second[0], L"Rediscovery returned a different object");
			Assert::IsTrue(first[0] == node->getSystemById("sys1"));
			Assert::AreEqual(size_t(1), node->getSystems().size());
			Assert::IsTrue(first[0] == node->getSystems()[0]);
		}

		TEST_METHOD(Updates_AreVisibleToEveryHolder) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto node = oshConnect.getNodeManager().addNode(local.root(), "admin", "admin");
			auto held = node->discoverSystems().front();

			local.setSystemName("Renamed System");
			node->discoverSystems();

			Assert::AreEqual("Renamed System"s, held->getSystemResource().getProperties().getName());
		}

		TEST_METHOD(DataStreams_DiscoveredThroughAnyHolder_AreShared) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto node = oshConnect.getNodeManager().addNode(local.root(), "admin", "admin");

			// Discover through a system obtained one way, then look through the node's cache
			auto dataStream = node->getSystemById("sys1")->discoverDataStreams().front();
			const auto& cached = node->getSystems().front()->getDataStreams();

			Assert::AreEqual(size_t(1), cached.size());
			Assert::IsTrue(cached[0] == dataStream);
			Assert::IsTrue(dataStream == node->getSystems().front()->getDataStreamById("ds1"));
		}

		TEST_METHOD(ParentLinks_PointAtTheOwningObjects) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto node = oshConnect.getNodeManager().addNode(local.root(), "admin", "admin");
			auto system = node->discoverSystems().front();
			auto dataStream = system->discoverDataStreams().front();

			Assert::IsTrue(system->getParentNode() == node);
			Assert::IsTrue(dataStream->getParentSystem() == system);
		}

		TEST_METHOD(Children_OutliveTheirNode) {
			LocalNodeServer local;
			std::shared_ptr<OSHConnect::OSHSystem> system;
			{
				OSHConnect::OSHConnect oshConnect;
				auto node = oshConnect.getNodeManager().addNode(local.root(), "admin", "admin");
				system = node->discoverSystems().front();
			}

			// The node is gone: the link back to it is empty instead of dangling...
			Assert::IsNull(system->getParentNode().get());
			// ...and the system still works, through the API it shares with the node
			auto dataStreams = system->discoverDataStreams();
			Assert::AreEqual(size_t(1), dataStreams.size());
			Assert::IsTrue(dataStreams[0]->getParentSystem() == system);
		}

		TEST_METHOD(GetNodeById_StaysValidAsNodesAreAdded) {
			OSHConnect::OSHConnect oshConnect;
			auto& nodeManager = oshConnect.getNodeManager();
			nodeManager.addNode("localhost:8282/sensorhub", "admin", "admin", "First", "node-first");
			auto first = nodeManager.getNodeById("node-first");

			// Adding nodes grows the manager's storage, which used to invalidate earlier results
			for (int i = 0; i < 100; ++i) {
				nodeManager.addNode("localhost:8282/sensorhub", "admin", "admin", "Node", "node-" + std::to_string(i));
			}

			Assert::IsTrue(first == nodeManager.getNodeById("node-first"));
			Assert::AreEqual("node-first"s, first->getUniqueId());
		}

		TEST_METHOD(AddNode_ReturnsTheManagedNode) {
			OSHConnect::OSHConnect oshConnect;
			auto& nodeManager = oshConnect.getNodeManager();
			auto added = nodeManager.addNode("localhost:8282/sensorhub", "admin", "admin", "Node", "node-001");

			Assert::IsTrue(added == nodeManager.getNodeById("node-001"));
			Assert::IsTrue(added == nodeManager.getNodes().front());
		}

		TEST_METHOD(Listener_ReconnectsAfterCallerReleasesEverything) {
			// The scenario that used to read freed memory: discovery results go out of scope while a handler,
			// which outlives them, still listens to one of their data streams
			ix::initNetSystem();
			std::unique_ptr<ix::WebSocketServer> server;
			int port = 0;
			for (int candidate = 18965; candidate < 19065 && !server; ++candidate) {
				auto attempt = std::make_unique<ix::WebSocketServer>(candidate, "127.0.0.1");
				if (attempt->listen().first) {
					server = std::move(attempt);
					port = candidate;
				}
			}
			Assert::IsNotNull(server.get(), L"No free port for the local websocket server");

			std::mutex mutex;
			std::condition_variable opened;
			int connections = 0;
			std::string uri;
			server->setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&, const ix::WebSocketMessagePtr& msg) {
				if (msg->type != ix::WebSocketMessageType::Open) return;
				std::lock_guard lock(mutex);
				connections++;
				uri = msg->openInfo.uri;
				opened.notify_all();
			});
			server->start();

			OSHConnect::OSHConnect oshConnect;
			auto* handler = oshConnect.getStreamManager().createDataStreamHandler([](const OSHConnect::Websocket::StreamEventArgs&) {});
			OSHConnect::Websocket::StreamListener* listener = nullptr;
			{
				TestHelper helper;
				auto node = std::make_shared<OSHConnect::OSHNode>("http://127.0.0.1:" + std::to_string(port) + "/sensorhub", "token", false);
				std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> api(node, &node->getConSysAPI());
				auto system = std::make_shared<OSHConnect::OSHSystem>(node, api, helper.createTestSystemObject());
				auto dataStream = std::make_shared<OSHConnect::OSHDataStream>(system, api,
					ConnectedSystemsAPI::DataModels::DataStreamBuilder(helper.createTestDataStreamObject()).withId("ds1").build());
				listener = handler->addDataStreamListener(dataStream);
			}

			// Connect twice: the second connect rebuilds the URL from the data stream, as a reconnect does
			handler->connect();
			listener->disconnect();
			listener->connect();
			{
				std::unique_lock lock(mutex);
				opened.wait_for(lock, std::chrono::seconds(10), [&] { return connections >= 2; });
			}

			oshConnect.getStreamManager().shutdownDataStreamHandler(handler);
			server->stop();

			Assert::AreEqual(2, connections);
			Assert::AreEqual("/sensorhub/api/datastreams/ds1/observations"s, uri);
		}
	};
}
