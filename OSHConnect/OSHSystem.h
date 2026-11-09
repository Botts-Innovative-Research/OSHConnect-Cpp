#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <CSAPI/DataModels/System.h>
#include <CSAPI/Query/ControlStreamsOfSystemQuery.h>
#include <CSAPI/Query/DataStreamsOfSystemQuery.h>
#include <CSAPI/ConnectedSystemsAPI.h>
#include <CSAPI/DataModels/ControlStreamBuilder.h>
#include <CSAPI/DataModels/ControlStream.h>
#include <CSAPI/DataModels/DataStreamBuilder.h>
#include <CSAPI/DataModels/DataStream.h>

#include "OSHDataStream.h"
#include "OSHControlStream.h"

namespace OSHConnect {
	class OSHNode;

	/// <summary>
	/// A system on an OpenSensorHub node.
	/// Systems are owned by their node and shared through std::shared_ptr, so every holder sees the same,
	/// up-to-date object. They own the data streams and control streams discovered through them, and are not copyable.
	/// All methods are thread-safe.
	/// </summary>
	class OSHSystem : public std::enable_shared_from_this<OSHSystem> {
	private:
		std::weak_ptr<OSHNode> parentNode;
		std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> conSysAPI;
		// Guards systemResource and the discovered streams. The parent link and API never change after construction.
		// Lock order: a system may lock its streams while holding this lock, never the reverse.
		mutable std::mutex mutex;
		ConnectedSystemsAPI::DataModels::System systemResource;
		std::vector<std::shared_ptr<OSHDataStream>> discoveredDataStreams{};
		std::vector<std::shared_ptr<OSHControlStream>> discoveredControlStreams{};

		std::shared_ptr<OSHDataStream> addOrUpdateDataStream(const ConnectedSystemsAPI::DataModels::DataStream& dataStreamResource) {
			if (!dataStreamResource.getId().has_value()) {
				throw std::invalid_argument("Data stream resource must have an ID to be added or updated.");
			}

			auto response = getConSysAPI().getDataStreamsAPI().getObservationSchema(dataStreamResource.getId().value());

			// Create a new builder with the data stream resource and the fetched observation schema
			ConnectedSystemsAPI::DataModels::DataStreamBuilder builder(dataStreamResource);
			builder.withSchema(response.getItem());
			auto dataStreamResourceNew = builder.build();

			// Find or add the data stream in one step, so concurrent discovery can't add it twice
			std::shared_ptr<OSHDataStream> existing;
			{
				std::lock_guard lock(mutex);
				for (auto& ds : discoveredDataStreams) {
					if (ds->getId() == dataStreamResourceNew.getId()) {
						existing = ds;
						break;
					}
				}
				if (!existing) {
					auto newDataStream = std::make_shared<OSHDataStream>(weak_from_this(), conSysAPI, dataStreamResourceNew);
					discoveredDataStreams.push_back(newDataStream);
					return newDataStream;
				}
			}

			// Update the existing data stream outside the system's lock
			existing->setDataStreamResource(dataStreamResourceNew);
			return existing;
		}

		std::shared_ptr<OSHControlStream> addOrUpdateControlStream(const ConnectedSystemsAPI::DataModels::ControlStream& controlStreamResource) {
			if (!controlStreamResource.getId().has_value()) {
				throw std::invalid_argument("Control stream resource must have an ID to be added or updated.");
			}

			auto response = getConSysAPI().getControlStreamsAPI().getControlStreamSchema(controlStreamResource.getId().value());

			// Create a new builder with the control stream resource and the fetched command schema
			ConnectedSystemsAPI::DataModels::ControlStreamBuilder builder(controlStreamResource);
			builder.withSchema(response.getItem());
			auto controlStreamResourceNew = builder.build();

			// Find or add the control stream in one step, so concurrent discovery can't add it twice
			std::shared_ptr<OSHControlStream> existing;
			{
				std::lock_guard lock(mutex);
				for (auto& cs : discoveredControlStreams) {
					if (cs->getId() == controlStreamResourceNew.getId()) {
						existing = cs;
						break;
					}
				}
				if (!existing) {
					auto newControlStream = std::make_shared<OSHControlStream>(weak_from_this(), conSysAPI, controlStreamResourceNew);
					discoveredControlStreams.push_back(newControlStream);
					return newControlStream;
				}
			}

			// Update the existing control stream outside the system's lock
			existing->setControlStreamResource(controlStreamResourceNew);
			return existing;
		}

		void setSystemResource(const ConnectedSystemsAPI::DataModels::System& resource) {
			std::lock_guard lock(mutex);
			systemResource = resource;
		}

		friend class OSHNode;

	public:
		/// <param name="parentNode">The node the system is on. May be empty.</param>
		/// <param name="conSysAPI">The API of the node, shared with the node.</param>
		/// <param name="systemResource">The system resource.</param>
		OSHSystem(std::weak_ptr<OSHNode> parentNode, std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> conSysAPI,
			const ConnectedSystemsAPI::DataModels::System& systemResource)
			: parentNode(std::move(parentNode)), conSysAPI(std::move(conSysAPI)), systemResource(systemResource) {
		}

		OSHSystem(const OSHSystem&) = delete;
		OSHSystem& operator=(const OSHSystem&) = delete;

		/// <summary>
		/// Query the node for the data streams associated with the system.
		/// New data streams will be added to the list of discovered data streams,
		/// and existing data streams will have their resources updated.
		/// </summary>
		/// <param name="query">Optional query string to filter data streams.</param>
		/// <returns>The list of discovered data streams.</returns>
		std::vector<std::shared_ptr<OSHDataStream>> discoverDataStreams(std::string query = "") {
			auto response = getConSysAPI().getDataStreamsAPI().getDataStreamsOfSystem(getId(), query);
			std::vector<std::shared_ptr<OSHDataStream>> discoveredStreams;
			if (response.isSuccessful()) {
				for (const auto& dataStreamResource : response.getItems()) {
					discoveredStreams.push_back(addOrUpdateDataStream(dataStreamResource));
				}
			}
			return discoveredStreams;
		}

		/// <summary>
		/// Query the node for the data streams associated with the system.
		/// New data streams will be added to the list of discovered data streams,
		/// and existing data streams will have their resources updated.
		/// </summary>
		/// <param name="query">Optional query to filter data streams.</param>
		/// <returns>The list of discovered data streams.</returns>
		std::vector<std::shared_ptr<OSHDataStream>> discoverDataStreams(const ConnectedSystemsAPI::Query::DataStreamsOfSystemQuery& query) {
			return discoverDataStreams(query.toString());
		}

		/// <summary>
		/// Query the node for the control streams associated with the system.
		/// New control streams will be added to the list of discovered control streams,
		/// and existing control streams will have their resources updated.
		/// </summary>
		/// <param name="query">Optional query string to filter control streams.</param>
		/// <returns>The list of discovered control streams.</returns>
		std::vector<std::shared_ptr<OSHControlStream>> discoverControlStreams(std::string query = "") {
			auto response = getConSysAPI().getControlStreamsAPI().getControlStreamsOfSystem(getId(), query);
			std::vector<std::shared_ptr<OSHControlStream>> discoveredStreams;
			if (response.isSuccessful()) {
				for (const auto& controlStreamResource : response.getItems()) {
					discoveredStreams.push_back(addOrUpdateControlStream(controlStreamResource));
				}
			}
			return discoveredStreams;
		}

		/// <summary>
		/// Query the node for the control streams associated with the system.
		/// New control streams will be added to the list of discovered control streams,
		/// and existing control streams will have their resources updated.
		/// </summary>
		/// <param name="query">Optional query to filter control streams.</param>
		/// <returns>The list of discovered control streams.</returns>
		std::vector<std::shared_ptr<OSHControlStream>> discoverControlStreams(const ConnectedSystemsAPI::Query::ControlStreamsOfSystemQuery& query) {
			return discoverControlStreams(query.toString());
		}

		/// <summary>
		/// Refresh the system properties from the server.
		/// </summary>
		/// <returns>True if the refresh was successful, false otherwise.</returns>
		bool refreshSystem() {
			auto response = getConSysAPI().getSystemsAPI().getSystemById(getId());
			if (response.isSuccessful()) {
				setSystemResource(response.getItem());
				return true;
			}
			return false;
		}

		/// <summary>
		/// Create a new data stream on the OpenSensorHub node using the provided data stream resource.
		/// </summary>
		/// <param name="dataStreamResource">The data stream resource to create.</param>
		/// <returns>The newly created data stream, or nullptr if creation failed.</returns>
		std::shared_ptr<OSHDataStream> createDataStream(const ConnectedSystemsAPI::DataModels::DataStream& dataStreamResource) {
			auto response = getConSysAPI().getDataStreamsAPI().createDataStream(getId(), dataStreamResource);

			for (const auto& [headerKey, headerValues] : response.getHeaders()) {
				for (const auto& headerValue : headerValues) {
					if (headerKey == "Location") {
						// Extract system ID from Location header if available.
						// The Location header is in the format: /datastreams/{dataStreamId}
						std::string location = headerValue;
						size_t lastSlashPos = location.find_last_of('/');
						if (lastSlashPos != std::string::npos && lastSlashPos + 1 < location.size()) {
							std::string newDataStreamId = location.substr(lastSlashPos + 1);
							return getDataStreamById(newDataStreamId);
						}
					}
				}
			}
			return nullptr;
		}

		/// <summary>
		/// Delete a data stream from the OpenSensorHub node and remove it from the list of discovered data streams.
		/// Holders of the data stream keep their reference, but it no longer belongs to this system.
		/// </summary>
		/// <param name="dataStreamId">The ID of the data stream to delete.</param>
		/// <param name="cascade">If true, all associated sub-resources hosted by the same server,
		///                       (sampling features, data streams, command streams, observations, and commands)
		///                       are also deleted.</param>
		/// <returns>True if the data stream was successfully deleted, false otherwise.</returns>
		bool deleteDataStream(const std::string& dataStreamId, const bool cascade = false) {
			auto response = getConSysAPI().getDataStreamsAPI().deleteDataStream(dataStreamId, cascade);
			if (response.isSuccessful()) {
				std::lock_guard lock(mutex);
				discoveredDataStreams.erase(std::remove_if(discoveredDataStreams.begin(), discoveredDataStreams.end(),
					[&dataStreamId](const std::shared_ptr<OSHDataStream>& ds) { return ds->getId() == dataStreamId; }),
					discoveredDataStreams.end());
				return true;
			}
			return false;
		}

		/// <summary>
		/// Create a new control stream on the OpenSensorHub node using the provided control stream resource.
		/// </summary>
		/// <param name="controlStreamResource">The control stream resource to create.</param>
		/// <returns>The newly created control stream, or nullptr if creation failed.</returns>
		std::shared_ptr<OSHControlStream> createControlStream(const ConnectedSystemsAPI::DataModels::ControlStream& controlStreamResource) {
			auto response = getConSysAPI().getControlStreamsAPI().createControlStream(getId(), controlStreamResource);

			for (const auto& [headerKey, headerValues] : response.getHeaders()) {
				for (const auto& headerValue : headerValues) {
					if (headerKey == "Location") {
						// Extract control stream ID from Location header if available.
						// The Location header is in the format: /controlstreams/{controlStreamId}
						std::string location = headerValue;
						size_t lastSlashPos = location.find_last_of('/');
						if (lastSlashPos != std::string::npos && lastSlashPos + 1 < location.size()) {
							std::string newControlStreamId = location.substr(lastSlashPos + 1);
							return getControlStreamById(newControlStreamId);
						}
					}
				}
			}
			return nullptr;
		}

		/// <summary>
		/// Delete a control stream from the OpenSensorHub node and remove it from the list of discovered control streams.
		/// Holders of the control stream keep their reference, but it no longer belongs to this system.
		/// </summary>
		/// <param name="controlStreamId">The ID of the control stream to delete.</param>
		/// <param name="cascade">If true, all associated commands will also be deleted.</param>
		/// <returns>True if the control stream was successfully deleted, false otherwise.</returns>
		bool deleteControlStream(const std::string& controlStreamId, const bool cascade = false) {
			auto response = getConSysAPI().getControlStreamsAPI().deleteControlStream(controlStreamId, cascade);
			if (response.isSuccessful()) {
				std::lock_guard lock(mutex);
				discoveredControlStreams.erase(std::remove_if(discoveredControlStreams.begin(), discoveredControlStreams.end(),
					[&controlStreamId](const std::shared_ptr<OSHControlStream>& cs) { return cs->getId() == controlStreamId; }),
					discoveredControlStreams.end());
				return true;
			}
			return false;
		}

		/// <summary>
		/// Update the system properties on the server.
		/// Note: After updating the system, the system properties are refreshed from the server,
		/// not set to the provided system properties.
		/// </summary>
		/// <returns>True if the update was successful, false otherwise.</returns>
		bool updateSystem(const ConnectedSystemsAPI::DataModels::System& updatedSystemResource) {
			if (auto response = getConSysAPI().getSystemsAPI().updateSystem(getId(), updatedSystemResource); response.isSuccessful()) {
				return refreshSystem();
			}
			return false;
		}

		/// <summary>
		/// Get a data stream by its ID and add it to the list of discovered data streams if not already present.
		/// If the data stream is already in the list of discovered data streams,
		/// it will be updated with the latest resource from the server.
		/// </summary>
		/// <param name="dataStreamId">The ID of the data stream to retrieve.</param>
		/// <returns>The data stream, or nullptr if not found.</returns>
		std::shared_ptr<OSHDataStream> getDataStreamById(const std::string& dataStreamId) {
			auto response = getConSysAPI().getDataStreamsAPI().getDataStreamById(dataStreamId);
			if (response.isSuccessful()) {
				return addOrUpdateDataStream(response.getItem());
			}
			return nullptr;
		}

		/// <summary>
		/// Get a control stream by its ID and add it to the list of discovered control streams if not already present.
		/// If the control stream is already in the list of discovered control streams,
		/// it will be updated with the latest resource from the server.
		/// </summary>
		/// <param name="controlStreamId">The ID of the control stream to retrieve.</param>
		/// <returns>The control stream, or nullptr if not found.</returns>
		std::shared_ptr<OSHControlStream> getControlStreamById(const std::string& controlStreamId) {
			auto response = getConSysAPI().getControlStreamsAPI().getControlStreamById(controlStreamId);
			if (response.isSuccessful()) {
				return addOrUpdateControlStream(response.getItem());
			}
			return nullptr;
		}

		/// <summary>
		/// The data streams discovered so far through this system, without querying the node.
		/// </summary>
		std::vector<std::shared_ptr<OSHDataStream>> getDataStreams() const {
			std::lock_guard lock(mutex);
			return discoveredDataStreams;
		}

		/// <summary>
		/// The control streams discovered so far through this system, without querying the node.
		/// </summary>
		std::vector<std::shared_ptr<OSHControlStream>> getControlStreams() const {
			std::lock_guard lock(mutex);
			return discoveredControlStreams;
		}

		/// <summary>
		/// The system resource representing this system.
		/// </summary>
		ConnectedSystemsAPI::DataModels::System getSystemResource() const {
			std::lock_guard lock(mutex);
			return systemResource;
		}
		/// <summary>
		/// The ID of the system. Same as systemResource.getId().
		/// </summary>
		std::string getId() const {
			std::lock_guard lock(mutex);
			return systemResource.getId();
		}
		/// <summary>
		/// The ConnectedSystemsAPI instance used to communicate with the server.
		/// Shared with the node, so it remains usable even if the node is removed.
		/// </summary>
		ConnectedSystemsAPI::ConSysAPI& getConSysAPI() const { return *conSysAPI; }

		/// <summary>
		/// The node the system is on, or nullptr if that node no longer exists.
		/// </summary>
		std::shared_ptr<OSHNode> getParentNode() const { return parentNode.lock(); }
	};
}
