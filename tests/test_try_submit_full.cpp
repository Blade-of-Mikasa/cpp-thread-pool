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

    // With a single worker, this task will sit in the queue while f1 is running.
    auto f2 = pool.submit([]() {
        // no-op
    });

    // Queue capacity is 1, so try_submit should fail fast here.
    auto f3 = pool.try_submit([]() {
        // would run if accepted
    });
    if (f3.has_value()) {
        std::cerr << "expected try_submit() to fail when the queue is full\n";
        return 1;
    }

    release.store(true);
    f1.get();
    f2.get();
    pool.shutdown();
    return 0;
}
