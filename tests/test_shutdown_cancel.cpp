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
    opts.queue_capacity = 4;
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

    // This future should be canceled by shutdown(cancel) because it is still pending in the queue.
    auto f2 = pool.submit([]() {
        // no-op
    });

    pool.shutdown(thread_pool::shutdown_mode::cancel);
    release.store(true);

    f1.get();

    try {
        f2.get();
        std::cerr << "expected pending task to be canceled by shutdown(cancel)\n";
        return 1;
    } catch (const thread_pool::task_canceled &) {
        // ok
    } catch (...) {
        std::cerr << "expected thread_pool::task_canceled\n";
        return 1;
    }

    pool.wait_idle();
    return 0;
}
