#pragma once

#include <string>
#include <stdexcept>
#include <ostream>

namespace OSHConnect::Constants {
	/// <summary>
	/// Enum representing the format of the request.
	/// </summary>
	enum class RequestFormat {
		JSON,
		OM_JSON,
		SWE_CSV,
		SWE_JSON,
		SWE_XML,
		SWE_BINARY,
		PLAIN_TEXT
	};

	/// <summary>
	/// The MIME type of the format, e.g., "application/om+json".
	/// This is not URL-encoded; QueryStringBuilder encodes it when used in a query string.
	/// </summary>
	inline std::string getMimeType(RequestFormat rf) {
		switch (rf) {
		case RequestFormat::JSON:        return "application/json";
		case RequestFormat::OM_JSON:     return "application/om+json";
		case RequestFormat::SWE_CSV:     return "application/swe+csv";
		case RequestFormat::SWE_JSON:    return "application/swe+json";
		case RequestFormat::SWE_XML:     return "application/swe+xml";
		case RequestFormat::SWE_BINARY:  return "application/swe+binary";
		case RequestFormat::PLAIN_TEXT:  return "text/plain";
		}
		throw std::invalid_argument("Unknown RequestFormat value");
	}

	inline std::ostream& operator<<(std::ostream& os, RequestFormat rf) {
		return os << getMimeType(rf);
	}

	inline std::string operator+(const std::string& lhs, RequestFormat rhs) {
		return lhs + getMimeType(rhs);
	}
}
