// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.
// Every .cpp file must include "pch.h" before anything else; MSVC skips any includes that come before it.

#ifndef PCH_H
#define PCH_H

// Standard library
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// Third-party headers used by CSAPI and OSHConnect. They are large and rarely change.
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "CppUnitTest.h"

// OSHConnect, which includes CSAPI. They change often, but nearly every test file includes them anyway, so a change to
// them recompiles every file either way; precompiling them makes edits to the tests themselves much faster.
#include "../OSHConnect/OSHConnect.h"

#endif //PCH_H
