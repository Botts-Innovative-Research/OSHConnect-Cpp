#include "pch.h"
#include "CppUnitTest.h"

#include <string>

#include "../OSHConnect/OSHConnect.h"
#include "../OSHConnect/OSHNode.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using ConnectedSystemsAPI::ConnectionState;

namespace OSHConnectTest
{
	/// <summary>
	/// Tests for OSHNode::checkConnection.
	/// Note: These tests require a running OpenSensorHub node at localhost:8282.
	/// </summary>
	TEST_CLASS(NodeConnectionTest)
	{
		static ConnectedSystemsAPI::ConnectionStatus check(const std::string& root) {
			OSHConnect::OSHConnect oshConnect("TestOSHConnect");
			auto node = oshConnect.getNodeManager().addNode(root, "admin", "admin");
			auto status = node.checkConnection();
			Logger::WriteMessage((root + ": " + toString(status.getState()) + " (HTTP " + std::to_string(status.getResponseCode())
				+ ") " + status.getMessage() + "\n").c_str());
			return status;
		}

	public:
		TEST_METHOD(LocalNode_IsConnected) {
			OSHConnect::OSHConnect oshConnect("TestOSHConnect");
			auto node = oshConnect.getNodeManager().addNode("http://localhost:8282/sensorhub", "admin", "admin");

			// Connected regardless of whether the node has any systems
			Assert::IsTrue(node.isConnected());
			Logger::WriteMessage(("Systems on the node: " + std::to_string(node.discoverSystems().size()) + "\n").c_str());
		}

		TEST_METHOD(WrongPath_IsNotConnectedSystemsApi) {
			auto status = check("http://localhost:8282/wrongpath");
			Assert::IsTrue(status.getState() == ConnectionState::NOT_CONNECTED_SYSTEMS_API);
		}

		TEST_METHOD(NothingListening_IsUnreachable) {
			auto status = check("http://localhost:9999/sensorhub");
			Assert::IsTrue(status.getState() == ConnectionState::UNREACHABLE);
			Assert::AreEqual(0, status.getResponseCode());
		}

		TEST_METHOD(UnknownHost_IsUnreachable) {
			auto status = check("http://oshconnect-test.invalid:8282/sensorhub");
			Assert::IsTrue(status.getState() == ConnectionState::UNREACHABLE);
		}
	};
}
