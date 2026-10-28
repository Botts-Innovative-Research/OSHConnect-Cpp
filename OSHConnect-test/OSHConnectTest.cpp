#include "pch.h"
#include "CppUnitTest.h"

#include <string>

#include "../OSHConnect/OSHConnect.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace OSHConnectTest
{
	TEST_CLASS(OSHConnectTest)
	{
	public:
		TEST_METHOD(Constructor_SetsName) {
			OSHConnect::OSHConnect oshConnect("TestOSHConnect");
			Assert::AreEqual(std::string("TestOSHConnect"), oshConnect.getName());
		}

		TEST_METHOD(GetNodeManager_ReturnsNodeManager) {
			OSHConnect::OSHConnect oshConnect("TestOSHConnect");
			Assert::IsNotNull(&oshConnect.getNodeManager());
		}
	};
}
