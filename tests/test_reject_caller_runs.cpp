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
    opts.on_queue_full = thread_pool::reject_policy::caller_runs;

    auto &pool = thread_pool::instance(opts);

    const auto caller_id = std::this_thread::get_id();
    std::atomic<bool> ran_on_caller{false};

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

    // When the queue is full, caller_runs should execute the task synchronously on this thread.
    auto f3 = pool.submit([&]() {
        ran_on_caller.store(std::this_thread::get_id() == caller_id);
    });
    f3.get();

    if (!ran_on_caller.load()) {
        std::cerr << "expected caller_runs policy to execute task on the calling thread\n";
        return 1;
    }

    release.store(true);
    f1.get();
    f2.get();
    pool.shutdown();
    return 0;
}
