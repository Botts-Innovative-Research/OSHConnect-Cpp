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
	/// A StreamHandler delivers data stream updates on its own thread, one at a time; a StreamListener delivers
	/// them and status changes on its network thread. In Unreal, hand them to the game thread before touching
	/// any UObject. Callbacks may call back into OSHConnect, e.g., to connect, disconnect, or shut down streams.
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