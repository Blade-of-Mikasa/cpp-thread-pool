#include "thread_pool.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace std::chrono_literals;

    thread_pool::options opts;
    opts.min_threads = 2;
    opts.max_threads = 2;
    opts.idle_timeout = 200ms;
    opts.queue_capacity = 0; // unbounded
    opts.on_queue_full = thread_pool::reject_policy::block;

    auto &pool = thread_pool::instance(opts);
    auto &pool_noarg = thread_pool::instance();
    if (&pool != &pool_noarg) {
        std::cerr << "expected instance() to return the initialized singleton\n";
        return 1;
    }

    auto sum_future = pool.submit([](int a, int b) { return a + b; }, 2, 3);
    if (sum_future.get() != 5) {
        std::cerr << "expected submit() to return a future with the correct value\n";
        return 1;
    }

    auto ex_future = pool.submit([]() -> int {
        throw std::runtime_error("boom");
    });

    try {
        (void)ex_future.get();
        std::cerr << "expected exception to propagate through future.get()\n";
        return 1;
    } catch (const std::runtime_error &) {
        // ok
    } catch (...) {
        std::cerr << "expected std::runtime_error\n";
        return 1;
    }

    pool.wait_idle();
    pool.shutdown();
    return 0;
}

