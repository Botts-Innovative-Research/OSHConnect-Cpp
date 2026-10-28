#include "pch.h"
#include "CppUnitTest.h"

#include <cstdint>
#include <string>
#include <vector>

#include "TestHelper.h"
#include "../OSHConnect/OSHDataStream.h"
#include "../OSHConnect/Websocket/StreamListener.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace OSHConnect;

namespace OSHConnectTest
{
	TEST_CLASS(StreamListenerTimestampTest)
	{
		// 2026-01-02T03:04:05Z
		static constexpr int64_t EPOCH_MILLIS = 1767323045000;

		/// <summary>
		/// Records the timestamp the listener determines for each message, without connecting anywhere.
		/// </summary>
		class TimestampListener : public Websocket::StreamListener {
		public:
			using Websocket::StreamListener::StreamListener;
			using Websocket::StreamListener::onStreamUpdate;

			int64_t timestamp = 0;
			Constants::RequestFormat format = Constants::RequestFormat::JSON;

			void onStreamUpdate(const Websocket::StreamEventArgs& args) override {
				timestamp = args.getTimestamp();
				format = args.getFormat();
			}
		};

		static int64_t timestampOf(const std::string& payload) {
			TestHelper helper;
			OSHDataStream dataStream(nullptr, helper.createTestDataStreamObject());
			TimestampListener listener(dataStream);
			listener.onStreamUpdate(std::vector<uint8_t>(payload.begin(), payload.end()));
			return listener.timestamp;
		}

		static std::string jsonWithTime(const std::string& time) {
			return R"({"datastream@id":"ds1","phenomenonTime":")" + time + R"(","result":{}})";
		}

	public:
		TEST_METHOD(Json_Milliseconds) {
			Assert::AreEqual(EPOCH_MILLIS + 678, timestampOf(jsonWithTime("2026-01-02T03:04:05.678Z")));
		}

		TEST_METHOD(Json_NoFractionalSeconds) {
			Assert::AreEqual(EPOCH_MILLIS, timestampOf(jsonWithTime("2026-01-02T03:04:05Z")));
		}

		TEST_METHOD(Json_OneFractionalDigit_IsTenths) {
			Assert::AreEqual(EPOCH_MILLIS + 900, timestampOf(jsonWithTime("2026-01-02T03:04:05.9Z")));
		}

		TEST_METHOD(Json_Microseconds_AreTruncatedToMilliseconds) {
			Assert::AreEqual(EPOCH_MILLIS + 123, timestampOf(jsonWithTime("2026-01-02T03:04:05.123456Z")));
		}

		TEST_METHOD(Json_Offset_IsConvertedToUtc) {
			Assert::AreEqual(EPOCH_MILLIS - 2 * 3600 * 1000, timestampOf(jsonWithTime("2026-01-02T03:04:05+02:00")));
		}

		TEST_METHOD(Json_InvalidTime_IsMinusOne) {
			Assert::AreEqual(int64_t(-1), timestampOf(jsonWithTime("yesterday")));
		}

		TEST_METHOD(Csv_FirstField) {
			Assert::AreEqual(EPOCH_MILLIS + 500, timestampOf("2026-01-02T03:04:05.5Z,34.76,-86.55,193.0\n"));
		}

		TEST_METHOD(Text_TrailingNewline_IsIgnored) {
			Assert::AreEqual(EPOCH_MILLIS, timestampOf("2026-01-02T03:04:05Z\n"));
		}

		TEST_METHOD(Xml_NoFractionalSeconds) {
			Assert::AreEqual(EPOCH_MILLIS, timestampOf("<swe:DataRecord><swe:time>2026-01-02T03:04:05Z</swe:time></swe:DataRecord>"));
		}
	};
}
