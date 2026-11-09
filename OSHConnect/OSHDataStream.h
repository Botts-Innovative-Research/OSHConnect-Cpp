#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <CSAPI/DataModels/DataStream.h>
#include <CSAPI/DataModels/DataStreamBuilder.h>
#include <CSAPI/DataModels/Observation.h>
#include <CSAPI/ConnectedSystemsAPI.h>
#include <CSAPI/Query/ObservationsOfDataStreamQuery.h>

namespace OSHConnect {
	class OSHSystem;

	/// <summary>
	/// A data stream of a system on an OpenSensorHub node.
	/// Data streams are owned by their system and shared through std::shared_ptr, so every holder sees the same,
	/// up-to-date object. They are not copyable.
	/// All methods are thread-safe.
	/// </summary>
	class OSHDataStream {
	private:
		std::weak_ptr<OSHSystem> parentSystem;
		std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> conSysAPI;
		// Guards dataStreamResource. The parent link and API never change after construction.
		mutable std::mutex mutex;
		ConnectedSystemsAPI::DataModels::DataStream dataStreamResource;

		/// <summary>
		/// Replaces the data stream resource. A resource without a schema keeps the schema this data stream already has:
		/// the server sends the schema separately, so a missing one means it couldn't be fetched, not that it's gone.
		/// </summary>
		void setDataStreamResource(const ConnectedSystemsAPI::DataModels::DataStream& resource) {
			std::lock_guard lock(mutex);
			if (resource.getSchema() || !dataStreamResource.getSchema()) {
				dataStreamResource = resource;
				return;
			}
			dataStreamResource = withSchema(resource, *dataStreamResource.getSchema());
		}

		/// <summary>
		/// A copy of the resource with the given schema.
		/// </summary>
		static ConnectedSystemsAPI::DataModels::DataStream withSchema(const ConnectedSystemsAPI::DataModels::DataStream& resource,
			const ConnectedSystemsAPI::DataModels::ObservationSchema& schema) {
			ConnectedSystemsAPI::DataModels::DataStreamBuilder builder(resource);
			builder.withSchema(schema);
			return builder.build();
		}

		friend class OSHSystem;

	public:
		/// <param name="parentSystem">The system the data stream belongs to. May be empty.</param>
		/// <param name="conSysAPI">The API of the node the data stream is on, shared with the node.</param>
		/// <param name="dataStreamResource">The data stream resource.</param>
		OSHDataStream(std::weak_ptr<OSHSystem> parentSystem, std::shared_ptr<ConnectedSystemsAPI::ConSysAPI> conSysAPI,
			const ConnectedSystemsAPI::DataModels::DataStream& dataStreamResource)
			: parentSystem(std::move(parentSystem)), conSysAPI(std::move(conSysAPI)), dataStreamResource(dataStreamResource) {
		}

		OSHDataStream(const OSHDataStream&) = delete;
		OSHDataStream& operator=(const OSHDataStream&) = delete;

		/// <summary>
		/// Query the node for the latest observations of this data stream with the specified parameters.
		/// </summary>
		std::vector<ConnectedSystemsAPI::DataModels::Observation> fetchObservations(std::string query = "") {
			auto response = getConSysAPI().getObservationsAPI().getObservationsOfDataStream(getId(), query);
			if (response.isSuccessful() && !response.getItems().empty()) {
				return response.getItems();
			}
			return {};
		}

		/// <summary>
		/// Query the node for the latest observations of this data stream with the specified parameters.
		/// </summary>
		std::vector<ConnectedSystemsAPI::DataModels::Observation> fetchObservations(const ConnectedSystemsAPI::Query::ObservationsOfDataStreamQuery& query) {
			return fetchObservations(query.toString());
		}

		/// <summary>
		/// Query the node for a specific observation of this data stream by its ID.
		/// </summary>
		/// <param name="observationId">The ID of the observation to fetch.</param>
		/// <returns>The observation with the specified ID, or an empty observation if not found.</returns>
		ConnectedSystemsAPI::DataModels::Observation fetchObservationById(const std::string& observationId) {
			auto response = getConSysAPI().getObservationsAPI().getObservationById(observationId);
			if (response.isSuccessful()) {
				return response.getItem();
			}
			return ConnectedSystemsAPI::DataModels::Observation{};
		}

		/// <summary>
		/// Refresh the data stream properties and observation schema from the server.
		/// If the schema can't be fetched, the data stream keeps the schema it had.
		/// </summary>
		/// <returns>True if the refresh was successful, false otherwise.</returns>
		bool refreshDataStream() {
			const std::string id = getId();
			auto response = getConSysAPI().getDataStreamsAPI().getDataStreamById(id);
			if (!response.isSuccessful()) {
				return false;
			}

			// The data stream resource doesn't include the schema, which has its own endpoint.
			// If it can't be fetched, setDataStreamResource keeps the schema the data stream already has.
			auto schemaResponse = getConSysAPI().getDataStreamsAPI().getObservationSchema(id);
			setDataStreamResource(schemaResponse.isSuccessful()
				? withSchema(response.getItem(), schemaResponse.getItem())
				: response.getItem());
			return true;
		}

		/// <summary>
		/// Create a new observation for this data stream on the OpenSensorHub node using the provided observation resource.
		/// </summary>
		/// <param name="observation">The observation resource to create.</param>
		/// <returns>The created observation if successful, or std::nullopt if creation failed.</returns>
		std::string createObservation(const ConnectedSystemsAPI::DataModels::Observation& observation) {
			auto response = getConSysAPI().getObservationsAPI().createObservation(getId(), observation);
			for (const auto& [headerKey, headerValues] : response.getHeaders()) {
				for (const auto& headerValue : headerValues) {
					if (headerKey == "Location") {
						// Extract observation ID from Location header if available.
						// The Location header is in the format: /observations/{observationId}
						std::string location = headerValue;
						size_t lastSlashPos = location.find_last_of('/');
						if (lastSlashPos != std::string::npos && lastSlashPos + 1 < location.size()) {
							std::string newObservationId = location.substr(lastSlashPos + 1);
							return newObservationId;
						}
					}
				}
			}
			return "";
		}

		/// <summary>
		/// Update an existing observation of this data stream on the OpenSensorHub node using the provided observation resource.
		/// </summary>
		/// <param name="observationId">The ID of the observation to update.</param>
		/// <param name="updatedObservation">The updated observation resource.</param>
		/// <returns>True if the update was successful, false otherwise.</returns>
		bool updateObservation(const std::string& observationId, const ConnectedSystemsAPI::DataModels::Observation& updatedObservation) {
			auto response = getConSysAPI().getObservationsAPI().updateObservation(observationId, updatedObservation);
			return response.isSuccessful();
		}

		/// <summary>
		/// Delete an existing observation of this data stream on the OpenSensorHub node by its ID.
		/// </summary>
		/// <param name="observationId">The ID of the observation to delete.</param>
		/// <returns>True if the deletion was successful, false otherwise.</returns>
		bool deleteObservation(const std::string& observationId) {
			auto response = getConSysAPI().getObservationsAPI().deleteObservation(observationId);
			return response.isSuccessful();
		}

		/// <summary>
		/// The ID of the data stream. Same as dataStreamResource.getId().
		/// </summary>
		std::string getId() const {
			std::lock_guard lock(mutex);
			return dataStreamResource.getId().value();
		}

		/// <summary>
		/// The data stream resource representing this data stream.
		/// Its observation schema (getSchema()) is null if the schema has never been fetched successfully;
		/// call refreshDataStream() to try again.
		/// </summary>
		ConnectedSystemsAPI::DataModels::DataStream getDataStreamResource() const {
			std::lock_guard lock(mutex);
			return dataStreamResource;
		}

		/// <summary>
		/// The ConnectedSystemsAPI instance used to communicate with the server.
		/// Shared with the node, so it remains usable even if the node is removed.
		/// </summary>
		ConnectedSystemsAPI::ConSysAPI& getConSysAPI() const { return *conSysAPI; }

		/// <summary>
		/// The system the data stream belongs to, or nullptr if that system no longer exists.
		/// </summary>
		std::shared_ptr<OSHSystem> getParentSystem() const { return parentSystem.lock(); }
	};
}