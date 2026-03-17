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
    opts.on_queue_full = thread_pool::reject_policy::throw_exception;

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

    try {
        (void)pool.submit([]() {
            // would run if accepted
        });
        std::cerr << "expected submit() to throw when the queue is full and policy is throw_exception\n";
        return 1;
    } catch (const thread_pool::task_rejected &) {
        // ok
    } catch (...) {
        std::cerr << "expected thread_pool::task_rejected\n";
        return 1;
    }

    release.store(true);
    f1.get();
    f2.get();
    pool.shutdown();
    return 0;
}
