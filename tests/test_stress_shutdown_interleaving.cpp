#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;

    thread_pool::options opts;
    opts.min_threads = 2;
    opts.max_threads = 4;
    opts.idle_timeout = 200ms;
    opts.queue_capacity = 64;
    opts.on_queue_full = thread_pool::reject_policy::block;

    auto &pool = thread_pool::instance(opts);

    std::atomic<bool> keep_running{true};
    std::vector<std::thread> producers;
    producers.reserve(4);

    for (int i = 0; i < 4; ++i) {
        producers.emplace_back([&]() {
            std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::uniform_int_distribution<int> op(0, 2);

            while (keep_running.load()) {
                try {
                    const int which = op(rng);
                    if (which == 0) {
                        (void)pool.submit([]() {
                            std::this_thread::sleep_for(1ms);
                        });
                    } else if (which == 1) {
                        (void)pool.try_submit([]() {
                            // no-op
                        });
                    } else {
                        (void)pool.submit_for(5ms, []() {
                            std::this_thread::sleep_for(1ms);
                        });
                    }
                } catch (...) {
                    // After shutdown(), submit/try_submit/submit_for may throw; ignore.
                }
            }
        });
    }

    std::this_thread::sleep_for(150ms);

    // Interleave shutdown with concurrent submissions; this should not deadlock.
    pool.shutdown(thread_pool::shutdown_mode::cancel);
    keep_running.store(false);

    for (auto &t : producers) {
        t.join();
    }

    pool.wait_idle();
    return 0;
}

