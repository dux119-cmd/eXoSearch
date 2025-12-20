#ifndef SAFE_QUEUE_H
#define SAFE_QUEUE_H

#include <condition_variable>
#include <queue>
#include <mutex>
#include <atomic>
#include <optional>
#include <chrono>

// ============================================================================
// Thread-Safe Queue
// ============================================================================

template <typename T>
class SafeQueue {
	std::queue<T> queue_;
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::atomic<bool> running_{true};

public:
	// 1. Perfect forwarding - constructs T in-place
        template<typename... Args>
	void emplace(Args&&... items){
    {
        std::scoped_lock lock(mutex_);
        queue_.emplace(std::forward<Args>(items)...);
    }
    cv_.notify_one();
}
	// 2. Universal reference for efficient push
	void push(T&& item);

	// 3. Non-blocking try_pop (more efficient when timeout not needed)
	[[nodiscard]] std::optional<T> try_pop();

	// 4. Blocking pop with timeout
	[[nodiscard]] std::optional<T> pop(std::chrono::milliseconds timeout);

	// 5. Blocking pop without timeout
	[[nodiscard]] std::optional<T> pop();

	void shutdown();

	// 6. Thread-safe size check
	[[nodiscard]] size_t size() const;

	// 7. Thread-safe empty check
	[[nodiscard]] bool empty() const;
};

#endif
