#include "pch.h"
#include "CppUnitTest.h"

#include <string>

#include "TestHelper.h"
#include "../OSHConnect/OSHNode.h"
#include "../OSHConnect/OSHSystem.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace OSHConnectTest {
	/// <summary>
	/// Tests for OSHConnect integration with an OpenSensorHub node.
	/// Note: These tests require a running OpenSensorHub node at localhost:8282
	/// with a system and data stream available for testing.
	/// </summary>
	TEST_CLASS(DataStreamIntegrationTest) {
		TestHelper helper = TestHelper();
		std::string dataStreamId;

		TEST_METHOD_INITIALIZE(ClassInitialize) {
			helper.createTestSystem();
			auto system = helper.node->getSystemById(helper.systemId);
			dataStreamId = helper.createTestDataStream();
		}

		TEST_METHOD_CLEANUP(MethodCleanup) {
			helper.cleanupTestSystem();
		}

		TEST_METHOD(CreateDataStream) {
			Assert::IsFalse(dataStreamId.empty());
		}

		TEST_METHOD(DiscoverDataStreams) {
			auto system = helper.node->getSystemById(helper.systemId);
			auto dataStreams = system->discoverDataStreams();
			Assert::IsFalse(dataStreams.empty());
		}

		TEST_METHOD(GetDataStreamById) {
			auto system = helper.node->getSystemById(helper.systemId);
			auto dataStreamOpt = system->getDataStreamById(dataStreamId);
			Assert::IsNotNull(dataStreamOpt.get());
		}

		TEST_METHOD(RefreshDataStream) {
			auto system = helper.node->getSystemById(helper.systemId);
			auto dataStreamOpt = system->getDataStreamById(dataStreamId);
			Assert::IsNotNull(dataStreamOpt.get());
			auto& dataStream = *dataStreamOpt;
			bool refreshed = dataStream.refreshDataStream();
			Assert::IsTrue(refreshed);
		}

		TEST_METHOD(DeleteDataStream) {
			auto system = helper.node->getSystemById(helper.systemId);
			bool deleted = system->deleteDataStream(dataStreamId);
			Assert::IsTrue(deleted);
			// Verify deletion
			auto dataStreamOpt = system->getDataStreamById(dataStreamId);
			Assert::IsNull(dataStreamOpt.get());
		}
	};
}
