#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <memory>

#include "OSHNode.h"
#include "CSAPI/Util/Utilities.h"

namespace OSHConnect {
	/// <summary>
	/// Manages the nodes of an OSHConnect instance.
	/// Nodes are shared: the manager and every caller holding a node refer to the same object,
	/// so systems discovered through any holder are visible to all of them.
	/// </summary>
	class NodeManager {
	private:
		std::vector<std::shared_ptr<OSHNode>> nodes;

	public:
		/// <summary>
		/// Add a node to OSHConnect.
		/// If a node with the same ID already exists, the node will not be added.
		/// </summary>
		/// <param name="node">The node to add.</param>
		/// <returns>The existing node if it was found, otherwise the added node.</returns>
		std::shared_ptr<OSHNode> addNode(const std::shared_ptr<OSHNode>& node) {
			if (!node) return nullptr;

			for (const auto& existingNode : nodes) {
				if (existingNode->getUniqueId() == node->getUniqueId()) {
					return existingNode;
				}
			}

			nodes.push_back(node);
			return node;
		}

		/// <summary>
		/// Add a node to OSHConnect.
		/// If a node with the same ID already exists, the node will not be added.
		/// </summary>
		/// <param name="root">The root URL of the OpenSensorHub server, i.e., localhost:8181/sensorhub.</param>
		/// <param name="authToken">The authentication token for the node.</param>
		/// <param name="isBasicAuth">True if using Basic authentication, false if using Bearer token.</param>
		/// <param name="name">The name of the node.</param>
		/// <param name="uniqueId">The unique ID of the node.</param>
		/// <returns>The existing node if it was found, otherwise the newly added node.</returns>
		std::shared_ptr<OSHNode> addNode(const std::string& root, const std::string& authToken, bool isBasicAuth, const std::string& name = "", const std::string& uniqueId = "") {
			return addNode(std::make_shared<OSHNode>(root, authToken, isBasicAuth, name, uniqueId));
		}

		/// <summary>
		/// Add a node to OSHConnect.
		/// If a node with the same ID already exists, the node will not be added.
		/// </summary>
		/// <param name="root">The root URL of the OpenSensorHub server, i.e., localhost:8181/sensorhub.</param>
		/// <param name="username">The username for Basic authentication.</param>
		/// <param name="password">The password for Basic authentication.</param>
		/// <param name="name">The name of the node.</param>
		/// <param name="uniqueId">The unique ID of the node.</param>
		/// <returns>The existing node if it was found, otherwise the newly added node.</returns>
		std::shared_ptr<OSHNode> addNode(const std::string& root, const std::string& username, const std::string& password, const std::string& name = "", const std::string& uniqueId = "") {
			std::string authToken = ConnectedSystemsAPI::Utilities::base64_encode(username + ":" + password);
			return addNode(std::make_shared<OSHNode>(root, authToken, true, name, uniqueId));
		}

		// Overload for a string literal password. Without it, a const char* password converts to bool
		// and selects the (root, authToken, isBasicAuth) overload instead.
		std::shared_ptr<OSHNode> addNode(const std::string& root, const std::string& username, const char* password, const std::string& name = "", const std::string& uniqueId = "") {
			return addNode(root, username, std::string(password), name, uniqueId);
		}

		/// <summary>
		/// Remove a node from OSHConnect by ID.
		/// Holders of the node keep their reference; the node is destroyed when the last one releases it.
		/// </summary>
		/// <param name="uniqueId">The unique ID of the node to remove.</param>
		void removeNode(const std::string& uniqueId) {
			nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
				[&uniqueId](const std::shared_ptr<OSHNode>& node) {
				return node->getUniqueId() == uniqueId;
			}), nodes.end());
		}

		/// <summary>
		/// Remove a node from OSHConnect.
		/// Holders of the node keep their reference; the node is destroyed when the last one releases it.
		/// </summary>
		/// <param name="node">The node to remove.</param>
		void removeNode(const OSHNode& node) {
			removeNode(node.getUniqueId());
		}

		/// <summary>
		/// Remove all nodes from OSHConnect.
		/// </summary>
		void removeAllNodes() {
			nodes.clear();
		}

		/// <summary>
		/// Get a node from OSHConnect by ID.
		/// </summary>
		/// <param name="uniqueId">The unique ID of the node to get.</param>
		/// <returns>The node if found, nullptr otherwise.</returns>
		std::shared_ptr<OSHNode> getNodeById(const std::string& uniqueId) const {
			for (const auto& node : nodes) {
				if (node->getUniqueId() == uniqueId) {
					return node;
				}
			}
			return nullptr;
		}

		/// <summary>
		/// Get a list of nodes in OSHConnect.
		/// </summary>
		/// <returns>A vector of all managed nodes.</returns>
		const std::vector<std::shared_ptr<OSHNode>>& getNodes() const {
			return nodes;
		}

		/// <summary>
		/// Shutdown the NodeManager and remove all nodes.
		/// </summary>
		void shutdown() {
			removeAllNodes();
		}
	};
}
