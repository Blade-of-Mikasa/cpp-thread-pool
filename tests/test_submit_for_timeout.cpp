#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    thread_pool::options opts;
    opts.min_threads = 1;
    opts.max_threads = 1;
    opts.idle_timeout = 1s;
    opts.queue_capacity = 1;
    opts.on_queue_full = thread_pool::reject_policy::block;

    auto &pool = thread_pool::instance(opts);

    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    auto f1 = pool.submit([&started, &release]() {
        started.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(5ms);
        }
    });

    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }

    auto f2 = pool.submit([]() {
        // no-op
    });

    const auto maybe_future = pool.submit_for(50ms, []() {
        // would run if accepted
    });

    if (maybe_future.has_value()) {
        std::cerr << "expected submit_for() to time out when the queue stays full\n";
        return 1;
    }

    release.store(true);
    f1.get();
    f2.get();
    pool.shutdown();
    return 0;
}
