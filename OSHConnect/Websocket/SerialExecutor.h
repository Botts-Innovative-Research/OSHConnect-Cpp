#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace OSHConnect::Websocket {
	/// <summary>
	/// Runs tasks one at a time, in the order they were posted, on a dedicated thread started on first use.
	/// Used to keep user callbacks and blocking control calls off the network threads.
	/// </summary>
	/// <remarks>
	/// stop() may be called from a task running on the executor's own thread: the thread is then detached and
	/// exits once that task returns. The thread's state is shared with it, so the executor object itself may be
	/// destroyed by that task.
	/// </remarks>
	class SerialExecutor {
	private:
		struct State {
			std::mutex mutex;
			std::condition_variable condition;
			std::deque<std::function<void()>> tasks;
			std::function<void()> onExit;
			std::thread::id threadId;
			bool stopping = false;
			bool exited = false;
		};

		std::shared_ptr<State> state = std::make_shared<State>();
		std::thread thread; // Guarded by state->mutex

		static void run(std::shared_ptr<State> s) {
			for (;;) {
				std::function<void()> task;
				{
					std::unique_lock lock(s->mutex);
					s->condition.wait(lock, [&s] { return s->stopping || !s->tasks.empty(); });
					if (s->stopping) break;
					task = std::move(s->tasks.front());
					s->tasks.pop_front();
				}
				try {
					task();
				}
				catch (...) {
					// Tasks report their own errors; never let one end the thread
				}
			}

			std::function<void()> onExit;
			{
				std::lock_guard lock(s->mutex);
				onExit = std::move(s->onExit);
				s->exited = true;
			}
			s->condition.notify_all();
			if (onExit) onExit();
		}

	public:
		SerialExecutor() = default;
		SerialExecutor(const SerialExecutor&) = delete;
		SerialExecutor& operator=(const SerialExecutor&) = delete;

		~SerialExecutor() {
			stop();
		}

		/// <summary>
		/// Queues a task. Returns false, without running it, if the executor has been stopped.
		/// </summary>
		bool post(std::function<void()> task) {
			std::lock_guard lock(state->mutex);
			if (state->stopping) return false;
			state->tasks.push_back(std::move(task));
			if (!thread.joinable() && !state->exited) {
				thread = std::thread(&SerialExecutor::run, state);
				state->threadId = thread.get_id();
			}
			state->condition.notify_one();
			return true;
		}

		/// <summary>
		/// True if called from a task running on the executor's thread.
		/// </summary>
		bool isCurrentThread() const {
			std::lock_guard lock(state->mutex);
			return state->threadId == std::this_thread::get_id();
		}

		/// <summary>
		/// Discards queued tasks and stops the thread, waiting for the running task to finish,
		/// unless called from that task, in which case the thread exits once the task returns.
		/// </summary>
		void stop() {
			std::thread toJoin;
			{
				std::lock_guard lock(state->mutex);
				state->stopping = true;
				state->tasks.clear();
				toJoin = std::move(thread);
			}
			state->condition.notify_all();

			if (toJoin.joinable()) {
				if (toJoin.get_id() == std::this_thread::get_id()) {
					toJoin.detach();
				}
				else {
					toJoin.join();
				}
				return;
			}

			// Already detached by an earlier stop() from its own thread: wait for it to exit, unless this is that thread
			std::unique_lock lock(state->mutex);
			if (state->threadId != std::this_thread::get_id()) {
				state->condition.wait(lock, [this] { return state->exited || state->threadId == std::thread::id(); });
			}
		}

		/// <summary>
		/// Runs the function on the executor's thread after it stops, or right away if it never started or already exited.
		/// Intended for use from a task that has just stopped the executor, e.g., to destroy the executor's owner.
		/// </summary>
		void runOnExit(std::function<void()> function) {
			{
				std::lock_guard lock(state->mutex);
				if (state->threadId != std::thread::id() && !state->exited) {
					state->onExit = std::move(function);
					return;
				}
			}
			function();
		}
	};
}
