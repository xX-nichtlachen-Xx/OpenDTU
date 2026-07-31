// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <deque>
#include <mutex>
#include <optional>

template <typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() = default;
    ThreadSafeQueue(const ThreadSafeQueue<T>&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue<T>&) = delete;

    ThreadSafeQueue(ThreadSafeQueue<T>&& other)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue = std::move(other._queue);
    }

    virtual ~ThreadSafeQueue() { }

    unsigned long size() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _queue.size();
    }

    std::optional<T> pop()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue.empty()) {
            return {};
        }
        T tmp = _queue.front();
        _queue.pop_front();
        return tmp;
    }

    void push(const T& item)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.push_back(item);
    }

    void pushFront(const T& item)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.push_front(item);
    }

    // Inserts right after the current front entry instead of replacing it as
    // the new front. Needed when called from a command's own handleResponse()
    // (i.e. before that command has actually been popped by the caller) so
    // the inserted item ends up first once the still-pending pop() happens.
    void insertAfterFront(const T& item)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.insert(_queue.begin() + (_queue.empty() ? 0 : 1), item);
    }

    T front()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _queue.front();
    }

protected:
    std::deque<T> _queue;
    mutable std::mutex _mutex;

private:
    // Moved out of public interface to prevent races between this
    // and pop().
    bool empty() const
    {
        return _queue.empty();
    }
};
