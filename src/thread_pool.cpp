#include "thread_pool.h"

thread_pool::thread_pool(const int num_thread) {
    for (int i = 0; i < num_thread; ++i) {
        auto lambda = [this]()-> void {
            while (true) {
                std::unique_lock<std::mutex> lock(mtx);
                condition.wait(lock, [this]() {
                    return (not tasks.empty()) || stop;
                });
                // 只要还有task就不能析构
                if (stop && tasks.empty()) {
                    return;
                }
                // 被 move 的对象可析构
                auto task(std::move(tasks.front()));
                tasks.pop();
                lock.unlock();
                task();
            }
        };
        threads.emplace_back(lambda);
    }
}

thread_pool::~thread_pool() {
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop = true;
    }
    condition.notify_all();
    for (auto &t: threads) {
        t.join();
    }
}
