#include "pch.h"
#include "CppUnitTest.h"

#include <string>
#include <vector>

#include "../OSHConnect/Constants/RequestFormat.h"
#include "../OSHConnect/Websocket/StreamEventArgs.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace OSHConnect;
using namespace std::string_literals;

namespace OSHConnectTest
{
	TEST_CLASS(StreamEventArgsTest)
	{
		// An observation as an OpenSensorHub node sends it over the websocket, in both JSON and O&M JSON formats
		static std::vector<uint8_t> observationPayload() {
			std::string json = R"({
				"datastream@id": "02pu5s52nksg",
				"phenomenonTime": "2026-10-28T17:14:26.023Z",
				"resultTime": "2026-10-28T17:14:26.023Z",
				"result": { "location": { "lat": 34.768, "lon": -86.552, "alt": 193.0 } }
			})";
			return std::vector<uint8_t>(json.begin(), json.end());
		}

		static Websocket::StreamEventArgs eventArgs(Constants::RequestFormat format) {
			return Websocket::StreamEventArgs(0, observationPayload(), format, nullptr);
		}

	public:
		TEST_METHOD(GetObservation_Json_Parses) {
			auto observation = eventArgs(Constants::RequestFormat::JSON).getObservation();
			Assert::IsTrue(observation.has_value());
			Assert::AreEqual("02pu5s52nksg"s, observation->getDataStreamId().value());
		}

		TEST_METHOD(GetObservation_OmJson_Parses) {
			auto observation = eventArgs(Constants::RequestFormat::OM_JSON).getObservation();
			Assert::IsTrue(observation.has_value());
			Assert::IsTrue(observation->getResult().getField("location")->isDataBlock());
		}

		TEST_METHOD(GetObservation_OtherFormats_ReturnNothing) {
			Assert::IsFalse(eventArgs(Constants::RequestFormat::SWE_JSON).getObservation().has_value());
			Assert::IsFalse(eventArgs(Constants::RequestFormat::SWE_CSV).getObservation().has_value());
		}
	};
}
