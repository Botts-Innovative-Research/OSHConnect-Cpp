#pragma once

#include <string>

#include "NodeManager.h"
#include "StreamManager.h"

namespace OSHConnect {
	/// <summary>
	/// Entry point of the library: manages the nodes (NodeManager) and data stream handlers (StreamManager).
	/// </summary>
	/// <remarks>
	/// Threading: NodeManager, OSHNode, OSHSystem, OSHDataStream, OSHControlStream, StreamManager, StreamHandler,
	/// and StreamListener are all thread-safe. Requests to a node block, so make them off any UI or game thread.
	/// Data stream updates and status changes are delivered on network threads; a StreamHandler delivers them
	/// one at a time. In Unreal, hand them to the game thread before touching any UObject.
	/// </remarks>
	class OSHConnect {
	private:
		std::string name;
		NodeManager nodeManager = NodeManager();
		StreamManager streamManager = StreamManager();

	public:
		explicit OSHConnect(const std::string& name = "OSHConnect") : name(name) {}

		/// <summary>
		/// The name of the OSHConnect instance.
		/// </summary>
		std::string getName() const { return name; }

		/// <summary>
		/// Get the NodeManager instance.
		/// </summary>
		NodeManager& getNodeManager() { return nodeManager; }

		/// <summary>
		/// Get the StreamManager instance.
		/// </summary>
		StreamManager& getStreamManager() { return streamManager; }
	};
}