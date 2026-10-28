#include "pch.h"
#include "CppUnitTest.h"

#include <chrono>
#include <string>
#include <vector>

#include "../OSHConnect/Constants/RequestFormat.h"
#include "../OSHConnect/Util/QueryStringBuilder.h"

#include <CSAPI/DataModels/TimeExtent.h>
#include <CSAPI/Util/TimeUtils.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace OSHConnect;
using namespace std::string_literals;

namespace OSHConnectTest
{
	TEST_CLASS(QueryStringBuilderTest)
	{
	public:
		TEST_METHOD(Empty_IsEmptyString) {
			Assert::AreEqual(""s, Util::QueryStringBuilder().build());
		}

		TEST_METHOD(MimeType_PlusIsEncodedOnce) {
			auto query = Util::QueryStringBuilder().withParameter("format", Constants::getMimeType(Constants::RequestFormat::OM_JSON));
			Assert::AreEqual("?format=application/om%2Bjson"s, query.build());
		}

		TEST_METHOD(MimeType_AllFormats_AreEncodedOnce) {
			using Constants::RequestFormat;
			const std::vector<std::pair<RequestFormat, std::string>> expected = {
				{ RequestFormat::JSON, "application/json" },
				{ RequestFormat::OM_JSON, "application/om%2Bjson" },
				{ RequestFormat::SWE_CSV, "application/swe%2Bcsv" },
				{ RequestFormat::SWE_JSON, "application/swe%2Bjson" },
				{ RequestFormat::SWE_XML, "application/swe%2Bxml" },
				{ RequestFormat::SWE_BINARY, "application/swe%2Bbinary" },
				{ RequestFormat::PLAIN_TEXT, "text/plain" },
			};
			for (const auto& [format, encoded] : expected) {
				auto query = Util::QueryStringBuilder().withParameter("format", Constants::getMimeType(format));
				Assert::AreEqual("?format="s + encoded, query.build());
			}
		}

		TEST_METHOD(MimeType_IsUnencoded) {
			Assert::AreEqual("application/swe+json"s, Constants::getMimeType(Constants::RequestFormat::SWE_JSON));
		}

		TEST_METHOD(Value_ReservedCharacters_AreEncoded) {
			auto query = Util::QueryStringBuilder().withParameter("q", "a b&c=d+e#f?g%"s);
			Assert::AreEqual("?q=a%20b%26c%3Dd%2Be%23f%3Fg%25"s, query.build());
		}

		TEST_METHOD(Value_CommasColonsAndSlashes_AreKept) {
			auto query = Util::QueryStringBuilder().withParameter("observedProperty", "http://example.com/a,http://example.com/b"s);
			Assert::AreEqual("?observedProperty=http://example.com/a,http://example.com/b"s, query.build());
		}

		TEST_METHOD(TimeExtent_Range_IsStartSlashEnd) {
			using ConnectedSystemsAPI::TimeUtils::parseTimeStringUTC;
			ConnectedSystemsAPI::DataModels::TimeExtent extent(
				parseTimeStringUTC("2026-01-02T03:04:05.678Z"), parseTimeStringUTC("2026-01-03T04:05:06.789Z"));

			auto query = Util::QueryStringBuilder().withParameter("phenomenonTime", extent);
			Assert::AreEqual("?phenomenonTime=2026-01-02T03:04:05.678Z/2026-01-03T04:05:06.789Z"s, query.build());
		}

		TEST_METHOD(ReplayRequest_IsEncoded) {
			// The parameters StreamListener sends for a replay request
			using ConnectedSystemsAPI::TimeUtils::parseTimeStringUTC;
			ConnectedSystemsAPI::DataModels::TimeExtent extent(parseTimeStringUTC("2026-01-02T03:04:05.678Z"),
				(std::chrono::system_clock::time_point::max)());

			auto query = Util::QueryStringBuilder()
				.withParameter("format", Constants::getMimeType(Constants::RequestFormat::SWE_JSON))
				.withParameter("phenomenonTime", extent)
				.withParameter("replaySpeed", 2.5);
			Assert::AreEqual("?format=application/swe%2Bjson&phenomenonTime=2026-01-02T03:04:05.678Z/..&replaySpeed=2.5"s, query.build());
		}
	};
}
