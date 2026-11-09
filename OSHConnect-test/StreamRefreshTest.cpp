#include "pch.h"
#include "CppUnitTest.h"

#include <memory>
#include <string>

#include "LocalNodeServer.h"
#include "../OSHConnect/OSHConnect.h"
#include "../OSHConnect/OSHNode.h"
#include "../OSHConnect/OSHSystem.h"
#include "../OSHConnect/OSHDataStream.h"
#include "../OSHConnect/OSHControlStream.h"

#include <CSAPI/DataModels/Component/DataRecord.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::string_literals;

namespace OSHConnectTest
{
	/// <summary>
	/// Tests that discovering or refreshing a data stream or control stream handles its schema correctly.
	/// The stream resource from the server doesn't include the schema, which has its own endpoint.
	/// </summary>
	TEST_CLASS(StreamRefreshTest)
	{
		/// <summary>
		/// The name of the only field of a DataRecord schema, or an empty string if there is no such schema.
		/// </summary>
		static std::string fieldName(const ConnectedSystemsAPI::DataModels::Component::DataComponent* component) {
			auto record = dynamic_cast<const ConnectedSystemsAPI::DataModels::Component::DataRecord*>(component);
			if (!record || record->getFields().size() != 1) return "";
			return record->getFields()[0]->getName().value_or("");
		}

		static std::string observationFieldName(const OSHConnect::OSHDataStream& dataStream) {
			auto resource = dataStream.getDataStreamResource();
			return resource.getSchema() ? fieldName(resource.getSchema()->getResultSchema()) : "";
		}

		static std::string commandFieldName(const OSHConnect::OSHControlStream& controlStream) {
			auto resource = controlStream.getControlStreamResource();
			return resource.getSchema() ? fieldName(resource.getSchema()->getParametersSchema()) : "";
		}

		static std::shared_ptr<OSHConnect::OSHSystem> discoverSystem(OSHConnect::OSHConnect& oshConnect, const LocalNodeServer& local) {
			auto node = oshConnect.getNodeManager().addNode(local.root(), "admin", "admin");
			return node->discoverSystems().front();
		}

	public:
		TEST_METHOD(RefreshDataStream_KeepsTheSchema) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto dataStream = discoverSystem(oshConnect, local)->discoverDataStreams().front();
			Assert::AreEqual("flag"s, observationFieldName(*dataStream));

			Assert::IsTrue(dataStream->refreshDataStream());
			Assert::AreEqual("flag"s, observationFieldName(*dataStream), L"Refresh dropped the observation schema");
		}

		TEST_METHOD(RefreshDataStream_PicksUpAChangedSchema) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto dataStream = discoverSystem(oshConnect, local)->discoverDataStreams().front();

			local.setSchemaFieldName("renamed");
			Assert::IsTrue(dataStream->refreshDataStream());
			Assert::AreEqual("renamed"s, observationFieldName(*dataStream));
		}

		TEST_METHOD(RefreshDataStream_KeepsTheOldSchema_WhenTheSchemaCantBeFetched) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto dataStream = discoverSystem(oshConnect, local)->discoverDataStreams().front();

			local.setSchemasAvailable(false);
			Assert::IsTrue(dataStream->refreshDataStream());
			Assert::AreEqual("flag"s, observationFieldName(*dataStream));
		}

		TEST_METHOD(RefreshControlStream_KeepsTheSchema) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto controlStream = discoverSystem(oshConnect, local)->discoverControlStreams().front();
			Assert::AreEqual("flag"s, commandFieldName(*controlStream));

			Assert::IsTrue(controlStream->refreshControlStream());
			Assert::AreEqual("flag"s, commandFieldName(*controlStream), L"Refresh dropped the command schema");
		}

		TEST_METHOD(RefreshControlStream_PicksUpAChangedSchema) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto controlStream = discoverSystem(oshConnect, local)->discoverControlStreams().front();

			local.setSchemaFieldName("renamed");
			Assert::IsTrue(controlStream->refreshControlStream());
			Assert::AreEqual("renamed"s, commandFieldName(*controlStream));
		}

		TEST_METHOD(RefreshControlStream_KeepsTheOldSchema_WhenTheSchemaCantBeFetched) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto controlStream = discoverSystem(oshConnect, local)->discoverControlStreams().front();

			local.setSchemasAvailable(false);
			Assert::IsTrue(controlStream->refreshControlStream());
			Assert::AreEqual("flag"s, commandFieldName(*controlStream));
		}

		// A failed schema fetch used to attach an empty schema, which looked valid and replaced a good one

		TEST_METHOD(DiscoverDataStreams_LeavesTheSchemaNull_WhenItCantBeFetched) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto system = discoverSystem(oshConnect, local);

			local.setSchemasAvailable(false);
			auto dataStream = system->discoverDataStreams().front();
			Assert::IsNull(dataStream->getDataStreamResource().getSchema(), L"A schema that couldn't be fetched should be null");

			// Refreshing once the schema is available fetches it
			local.setSchemasAvailable(true);
			Assert::IsTrue(dataStream->refreshDataStream());
			Assert::AreEqual("flag"s, observationFieldName(*dataStream));
		}

		TEST_METHOD(RediscoverDataStreams_KeepsTheSchema_WhenItCantBeFetched) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto system = discoverSystem(oshConnect, local);
			auto dataStream = system->discoverDataStreams().front();

			local.setSchemasAvailable(false);
			system->discoverDataStreams();
			Assert::AreEqual("flag"s, observationFieldName(*dataStream), L"Rediscovery replaced the schema");
		}

		TEST_METHOD(DiscoverControlStreams_LeavesTheSchemaNull_WhenItCantBeFetched) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto system = discoverSystem(oshConnect, local);

			local.setSchemasAvailable(false);
			auto controlStream = system->discoverControlStreams().front();
			Assert::IsNull(controlStream->getControlStreamResource().getSchema(), L"A schema that couldn't be fetched should be null");

			local.setSchemasAvailable(true);
			Assert::IsTrue(controlStream->refreshControlStream());
			Assert::AreEqual("flag"s, commandFieldName(*controlStream));
		}

		TEST_METHOD(RediscoverControlStreams_KeepsTheSchema_WhenItCantBeFetched) {
			LocalNodeServer local;
			OSHConnect::OSHConnect oshConnect;
			auto system = discoverSystem(oshConnect, local);
			auto controlStream = system->discoverControlStreams().front();

			local.setSchemasAvailable(false);
			system->discoverControlStreams();
			Assert::AreEqual("flag"s, commandFieldName(*controlStream), L"Rediscovery replaced the schema");
		}
	};
}
