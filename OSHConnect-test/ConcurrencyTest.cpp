#include "pch.h"
#include "CppUnitTest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
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
	/// <summary>
	/// Tests that OSHConnect's objects can be used from several threads at once.
	/// These are stress tests: they can't prove the absence of data races, but they exercise every shared path
	/// and check the guarantees that matter to callers (one object per resource, one update at a time).
	/// </summary>
	TEST_CLASS(ConcurrencyTest)
	{
		static constexpr int THREADS = 8;

		template<typename Work>
		static void runOnThreads(Work work) {
			std::vector<std::thread> threads;
			for (int i = 0; i < THREADS; ++i) {
				threads.emplace_back([&work, i] { work(i); });
			}
			for (auto& thread : threads) {
				thread.join();
			}
		}

	public:
		TEST_METHOD(ConcurrentDiscovery_SharesOneObjectPerResource) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto node = oshConnect.getNodeManager().addNode(local.root(), "admin", "admin");

			std::mutex seenMutex;
			std::set<OSHConnect::OSHSystem*> seenSystems;
			std::set<OSHConnect::OSHDataStream*> seenDataStreams;

			runOnThreads([&](int thread) {
				for (int i = 0; i < 20; ++i) {
					auto systems = node->discoverSystems();
					auto dataStreams = systems.at(0)->discoverDataStreams();

					// Readers, interleaved with the writers above on other threads
					node->setName("Node " + std::to_string(thread));
					auto name = node->getName();
					auto resource = systems[0]->getSystemResource();
					auto cachedStreams = systems[0]->getDataStreams();
					auto id = dataStreams.at(0)->getId();

					std::lock_guard lock(seenMutex);
					seenSystems.insert(systems[0].get());
					seenDataStreams.insert(dataStreams[0].get());
				}
			});

			Assert::AreEqual(size_t(1), seenSystems.size(), L"Concurrent discovery created more than one system object");
			Assert::AreEqual(size_t(1), seenDataStreams.size(), L"Concurrent discovery created more than one data stream object");
			Assert::AreEqual(size_t(1), node->getSystems().size());
			Assert::AreEqual(size_t(1), node->getSystems()[0]->getDataStreams().size());
		}

		TEST_METHOD(ConcurrentAddNode_AddsEachNodeOnce) {
			OSHConnect::OSHConnect oshConnect;
			auto& nodeManager = oshConnect.getNodeManager();

			std::mutex addedMutex;
			std::set<OSHConnect::OSHNode*> sharedNodeInstances;

			runOnThreads([&](int thread) {
				for (int i = 0; i < 50; ++i) {
					// Every thread adds the same node, plus nodes of its own
					auto shared = nodeManager.addNode("localhost:8282/sensorhub", "admin", "admin", "Shared", "node-shared");
					nodeManager.addNode("localhost:8282/sensorhub", "admin", "admin", "Own", "node-" + std::to_string(thread) + "-" + std::to_string(i));
					auto lookedUp = nodeManager.getNodeById("node-shared");
					auto snapshot = nodeManager.getNodes();

					std::lock_guard lock(addedMutex);
					sharedNodeInstances.insert(shared.get());
					sharedNodeInstances.insert(lookedUp.get());
				}
				nodeManager.removeNode("node-" + std::to_string(thread) + "-0");
			});

			Assert::AreEqual(size_t(1), sharedNodeInstances.size(), L"The same node was added more than once");
			Assert::AreEqual(size_t(1 + THREADS * 49), nodeManager.getNodes().size());
		}

		TEST_METHOD(StreamHandler_DeliversOneUpdateAtATime) {
			// A local websocket server sends a burst of messages on each connection as soon as it opens,
			// so the handler's listeners receive them on separate network threads at the same time
			ix::initNetSystem();
			std::unique_ptr<ix::WebSocketServer> server;
			int port = 0;
			for (int candidate = 19165; candidate < 19265 && !server; ++candidate) {
				auto attempt = std::make_unique<ix::WebSocketServer>(candidate, "127.0.0.1");
				if (attempt->listen().first) {
					server = std::move(attempt);
					port = candidate;
				}
			}
			Assert::IsNotNull(server.get(), L"No free port for the local websocket server");

			static constexpr int STREAMS = 3;
			static constexpr int MESSAGES = 200;
			server->setOnClientMessageCallback([](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& webSocket, const ix::WebSocketMessagePtr& msg) {
				if (msg->type != ix::WebSocketMessageType::Open) return;
				for (int i = 0; i < MESSAGES; ++i) {
					webSocket.sendText(R"({"datastream@id":"ds","phenomenonTime":"2026-01-02T03:04:05.678Z","result":{"n":)" + std::to_string(i) + "}}");
				}
			});
			server->start();

			std::atomic<int> inside{ 0 };
			std::atomic<int> maxInside{ 0 };
			std::atomic<int> received{ 0 };
			std::mutex doneMutex;
			std::condition_variable done;

			OSHConnect::OSHConnect oshConnect;
			auto* handler = oshConnect.getStreamManager().createDataStreamHandler([&](const OSHConnect::Websocket::StreamEventArgs&) {
				int now = ++inside;
				int previous = maxInside.load();
				while (now > previous && !maxInside.compare_exchange_weak(previous, now)) {}
				// Widen the window in which an unsynchronized second call would overlap.
				// Busy-wait: sleep_for rounds up to the OS timer tick (about 15 ms on Windows).
				auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
				while (std::chrono::steady_clock::now() < until) {}
				--inside;
				if (++received == STREAMS * MESSAGES) {
					std::lock_guard lock(doneMutex);
					done.notify_all();
				}
			});

			TestHelper helper;
			auto node = std::make_shared<OSHConnect::OSHNode>("http://127.0.0.1:" + std::to_string(port) + "/sensorhub", "token", false);
			std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> api(node, &node->getConSysAPI());
			auto system = std::make_shared<OSHConnect::OSHSystem>(node, api, helper.createTestSystemObject());
			for (int i = 0; i < STREAMS; ++i) {
				handler->addDataStreamListener(std::make_shared<OSHConnect::OSHDataStream>(system, api,
					ConnectedSystemsAPI::DataModels::DataStreamBuilder(helper.createTestDataStreamObject()).withId("ds" + std::to_string(i)).build()));
			}
			handler->connect();

			{
				std::unique_lock lock(doneMutex);
				done.wait_for(lock, std::chrono::seconds(20), [&] { return received.load() >= STREAMS * MESSAGES; });
			}
			oshConnect.getStreamManager().shutdownDataStreamHandler(handler);
			server->stop();

			Logger::WriteMessage(("Received " + std::to_string(received.load()) + " updates, at most "
				+ std::to_string(maxInside.load()) + " at once\n").c_str());
			Assert::AreEqual(STREAMS * MESSAGES, received.load());
			Assert::AreEqual(1, maxInside.load(), L"onStreamUpdate was called concurrently");
		}

		TEST_METHOD(StreamHandler_ConcurrentAddRemoveAndSettings) {
			TestHelper helper;
			auto node = std::make_shared<OSHConnect::OSHNode>("http://127.0.0.1:1/sensorhub", "token", false);
			std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> api(node, &node->getConSysAPI());
			auto system = std::make_shared<OSHConnect::OSHSystem>(node, api, helper.createTestSystemObject());

			std::vector<std::shared_ptr<OSHConnect::OSHDataStream>> dataStreams;
			for (int i = 0; i < THREADS; ++i) {
				dataStreams.push_back(std::make_shared<OSHConnect::OSHDataStream>(system, api,
					ConnectedSystemsAPI::DataModels::DataStreamBuilder(helper.createTestDataStreamObject()).withId("ds" + std::to_string(i)).build()));
			}

			OSHConnect::OSHConnect oshConnect;
			auto* handler = oshConnect.getStreamManager().createDataStreamHandler([](const OSHConnect::Websocket::StreamEventArgs&) {});

			runOnThreads([&](int thread) {
				for (int i = 0; i < 50; ++i) {
					// Every thread adds a shared stream and its own, changes settings, then removes its own
					handler->addDataStreamListener(dataStreams[0]);
					handler->addDataStreamListener(dataStreams[thread]);
					handler->setReplaySpeed(1.0 + thread);
					handler->getDataStreamListenerCount();
					if (thread != 0) handler->shutdownDataStreamListener(*dataStreams[thread]);
				}
			});

			Assert::AreEqual(size_t(1), handler->getDataStreamListenerCount(), L"Expected only the shared stream to remain");
			oshConnect.getStreamManager().shutdownDataStreamHandler(handler);
		}
	};
}
