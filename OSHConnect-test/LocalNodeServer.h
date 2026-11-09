#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <httplib.h>

namespace OSHConnectTest {
	/// <summary>
	/// A local HTTP server that stands in for an OpenSensorHub node with one system (sys1), one data stream (ds1),
	/// and one control stream (cs1).
	/// </summary>
	class LocalNodeServer {
	private:
		httplib::Server server;
		std::thread thread;
		int port = 0;
		mutable std::mutex nameMutex;
		std::string systemName = "System One";
		std::string schemaFieldName = "flag";
		bool schemasAvailable = true;

		std::string system() const {
			std::lock_guard lock(nameMutex);
			return R"({"type":"Feature","id":"sys1","properties":{"featureType":"http://www.w3.org/ns/sosa/Sensor","uid":"urn:test:sys1","name":")"
				+ systemName + R"("}})";
		}

		/// <summary>
		/// Answers a schema request with the given schema, or a server error while schemas are unavailable.
		/// </summary>
		void schema(httplib::Response& res, const std::string& before, const std::string& after) const {
			std::lock_guard lock(nameMutex);
			if (!schemasAvailable) {
				res.status = 500;
				return;
			}
			res.set_content(before + schemaFieldName + after, "application/json");
		}

	public:
		LocalNodeServer() {
			server.Get("/sensorhub/api/systems", [this](const httplib::Request&, httplib::Response& res) {
				res.set_content("{\"items\":[" + system() + "]}", "application/json");
			});
			server.Get("/sensorhub/api/systems/sys1", [this](const httplib::Request&, httplib::Response& res) {
				res.set_content(system(), "application/json");
			});
			server.Get("/sensorhub/api/systems/sys1/datastreams", [](const httplib::Request&, httplib::Response& res) {
				res.set_content(R"({"items":[{"id":"ds1","name":"Data Stream One","outputName":"out1","system@id":"sys1"}]})", "application/json");
			});
			server.Get("/sensorhub/api/datastreams/ds1", [](const httplib::Request&, httplib::Response& res) {
				res.set_content(R"({"id":"ds1","name":"Data Stream One","outputName":"out1","system@id":"sys1"})", "application/json");
			});
			server.Get("/sensorhub/api/datastreams/ds1/schema", [this](const httplib::Request&, httplib::Response& res) {
				schema(res, R"({"obsFormat":"application/om+json","resultSchema":{"type":"DataRecord","name":"rec",
					"fields":[{"type":"Boolean","name":")", R"("}]}})");
			});
			server.Get("/sensorhub/api/systems/sys1/controlstreams", [](const httplib::Request&, httplib::Response& res) {
				res.set_content(R"({"items":[{"id":"cs1","name":"Control Stream One","inputName":"in1","system@id":"sys1"}]})", "application/json");
			});
			server.Get("/sensorhub/api/controlstreams/cs1", [](const httplib::Request&, httplib::Response& res) {
				res.set_content(R"({"id":"cs1","name":"Control Stream One","inputName":"in1","system@id":"sys1"})", "application/json");
			});
			server.Get("/sensorhub/api/controlstreams/cs1/schema", [this](const httplib::Request&, httplib::Response& res) {
				schema(res, R"({"commandFormat":"application/json","parametersSchema":{"type":"DataRecord","name":"params",
					"fields":[{"type":"Boolean","name":")", R"("}]}})");
			});
			port = server.bind_to_any_port("127.0.0.1");
			thread = std::thread([this] { server.listen_after_bind(); });
			server.wait_until_ready();
		}

		LocalNodeServer(const LocalNodeServer&) = delete;
		LocalNodeServer& operator=(const LocalNodeServer&) = delete;

		~LocalNodeServer() {
			server.stop();
			if (thread.joinable()) thread.join();
		}

		/// <summary>
		/// The node's root URL, e.g., http://127.0.0.1:12345/sensorhub.
		/// </summary>
		std::string root() const { return "http://127.0.0.1:" + std::to_string(port) + "/sensorhub"; }

		/// <summary>
		/// Changes the name the server reports for sys1.
		/// </summary>
		void setSystemName(const std::string& name) {
			std::lock_guard lock(nameMutex);
			systemName = name;
		}

		/// <summary>
		/// Changes the name of the only field in the schemas of ds1 and cs1.
		/// </summary>
		void setSchemaFieldName(const std::string& name) {
			std::lock_guard lock(nameMutex);
			schemaFieldName = name;
		}

		/// <summary>
		/// Makes the schema endpoints of ds1 and cs1 fail with a server error, or work again.
		/// </summary>
		void setSchemasAvailable(bool available) {
			std::lock_guard lock(nameMutex);
			schemasAvailable = available;
		}
	};
}
