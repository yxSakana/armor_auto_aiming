/**
 * @project_name auto_aim
 * @file safe_container.h
 * @brief
 * @author yx
 * @date 2023-12-09 21:04:35
 */

#ifndef AUTO_AIM_THREADSAFE_QUEUE_H
#define AUTO_AIM_THREADSAFE_QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

namespace armor_auto_aim {
template <typename DataType>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() =default;

    void push(DataType new_data) {
        std::shared_ptr<DataType> data(std::make_shared<DataType>(std::move(new_data)));
        {
            std::lock_guard<std::mutex> lk(m_queue_mutex);
            m_queue.push(data);
        }
        m_condition_variable.notify_one();
    }

    std::shared_ptr<DataType> waitPop() {
        std::unique_lock<std::mutex> lk(m_queue_mutex);
        m_condition_variable.wait(lk, [this](){ return !m_queue.empty(); });
        std::shared_ptr<DataType> data = m_queue.front();
        m_queue.pop();
        return data;
    }

    void waitPop(DataType& data) {
        std::unique_lock<std::mutex> lk(m_queue_mutex);
        m_condition_variable.wait(lk, [this](){ return !m_queue.empty(); });
        data = std::move(*m_queue.front());
        m_queue.pop();
    }

    std::shared_ptr<DataType> tryPop() {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        if (m_queue.empty()) return nullptr;

        std::shared_ptr<DataType> data = m_queue.front();
        m_queue.pop();
        return data;
    }

    bool tryPop(DataType& data) {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        if (m_queue.empty()) return false;

        data = std::move(*m_queue.front());
        m_queue.pop();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        return m_queue.empty();
    }

    int size() const {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        return m_queue.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_queue_mutex);
        while (!m_queue.empty()) {
            m_queue.pop();
        }
    }

private:
    mutable std::mutex m_queue_mutex;
    std::condition_variable m_condition_variable;
    std::queue<std::shared_ptr<DataType>> m_queue;
};
}

#endif //AUTO_AIM_THREADSAFE_QUEUE_H
