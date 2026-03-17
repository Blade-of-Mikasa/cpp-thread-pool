#include "thread_pool.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    // 创建/获取全局唯一线程池实例：最小 2，最大 6，空闲 500ms 后允许缩回到最小值。
    auto &pool = thread_pool::instance(2, 6, 500ms);
    std::mutex output_mutex;

    // 提交一波短任务，观察线程数在突发负载下的扩容效果。
    for (int i = 0; i < 12; ++i) {
        (void)pool.submit([&pool, &output_mutex](int task_id) {
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::cout << "task " << task_id
                          << " running on " << std::this_thread::get_id()
                          << ", total threads = " << pool.size()
                          << ", busy threads = " << pool.busy_size()
                          << '\n';
            }

            std::this_thread::sleep_for(300ms);
        }, i);
    }

    // 突发任务结束后，线程数通常会保持一段时间（直到 idle_timeout 触发缩容）。
    std::this_thread::sleep_for(900ms);
    {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << "after burst, current threads = " << pool.size() << '\n';
    }

    // 空闲一段时间后，线程池缩回到最小线程数。
    std::this_thread::sleep_for(1500ms);
    {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << "after idle shrink, current threads = " << pool.size() << '\n';
    }

    return 0;
}
