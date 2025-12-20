#include "safe_queue.h"

// ============================================================================
// Thread-Safe Queue
// ============================================================================

// 2. Universal reference for efficient push
template <class T>
void SafeQueue<T>::push(T&& item)
{
	{
		std::scoped_lock lock(mutex_);
		queue_.push(std::forward<T>(item));
	}
	cv_.notify_one();
}

// 3. Non-blocking try_pop (more efficient when timeout not needed)
template <class T>
[[nodiscard]] std::optional<T> SafeQueue<T>::try_pop()
{
	std::scoped_lock lock(mutex_);
	if (queue_.empty()) {
		return std::nullopt;
	}
	T item = std::move(queue_.front());
	queue_.pop();
	return item;
}

// 4. Blocking pop with timeout
template <class T>
[[nodiscard]] std::optional<T> SafeQueue<T>::pop(std::chrono::milliseconds timeout)
{
	std::unique_lock lock(mutex_);
	if (!cv_.wait_for(lock, timeout, [this] {
		    return !queue_.empty() ||
		           !running_.load(std::memory_order_relaxed);
	    })) {
		return std::nullopt;
	}

	if (queue_.empty()) {
		return std::nullopt;
	}

	T item = std::move(queue_.front());
	queue_.pop();
	return item;
}

// 5. Blocking pop without timeout
template <class T>
[[nodiscard]] std::optional<T> SafeQueue<T>::pop()
{
	std::unique_lock lock(mutex_);
	cv_.wait(lock, [this] {
		return !queue_.empty() || !running_.load(std::memory_order_relaxed);
	});

	if (queue_.empty()) {
		return std::nullopt;
	}

	T item = std::move(queue_.front());
	queue_.pop();
	return item;
}

template <class T>
void SafeQueue<T>::shutdown()
{
	running_.store(false, std::memory_order_relaxed);
	cv_.notify_all();
}

// 6. Thread-safe size check
template <class T>
[[nodiscard]] size_t SafeQueue<T>::size() const
{
	std::scoped_lock lock(mutex_);
	return queue_.size();
}

// 7. Thread-safe empty check
template <class T>
[[nodiscard]] bool SafeQueue<T>::empty() const
{
	std::scoped_lock lock(mutex_);
	return queue_.empty();
}

// Explicit instantiations
#include "command_t.h"
template class SafeQueue<Command>;
