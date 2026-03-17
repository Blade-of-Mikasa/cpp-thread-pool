#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    // 使用较短的 idle_timeout，让测试在合理时间内看到“缩容”效果。
    auto &pool = thread_pool::instance(1, 4, 150ms);
    std::atomic<int> finished_tasks{0};

    // 提交一波任务，制造负载，期望线程池扩容到 > 1。
    for (int i = 0; i < 8; ++i) {
        (void)pool.submit([&finished_tasks]() {
            std::this_thread::sleep_for(120ms);
            ++finished_tasks;
        });
    }

    // 给线程池一点时间做扩容（线程创建 + worker 抢到任务）。
    std::this_thread::sleep_for(50ms);
    if (pool.size() <= 1) {
        std::cerr << "expected the pool to scale up under load\n";
        return 1;
    }

    while (finished_tasks.load() != 8) {
        std::this_thread::sleep_for(20ms);
    }

    std::this_thread::sleep_for(500ms);
    if (pool.size() != 1) {
        std::cerr << "expected the pool to shrink back to min threads\n";
        return 1;
    }

    if (pool.pending_size() != 0) {
        std::cerr << "expected no pending tasks after all work completed\n";
        return 1;
    }

    return 0;
}
