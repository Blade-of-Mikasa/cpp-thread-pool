#include "thread_pool.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    thread_pool::options opts;
    opts.min_threads = 2;
    opts.max_threads = 4;
    opts.idle_timeout = 200ms;
    opts.queue_capacity = 0;
    opts.on_queue_full = thread_pool::reject_policy::block;
    opts.enable_metrics = true;

    auto &pool = thread_pool::instance(opts);

    constexpr int task_count = 50;
    for (int i = 0; i < task_count; ++i) {
        (void)pool.submit([]() {
            std::this_thread::sleep_for(2ms);
        });
    }

    pool.wait_idle();

    const auto m = pool.metrics();
    if (m.submitted_total != task_count || m.started_total != task_count || m.completed_total != task_count) {
        std::cerr << "expected submitted/started/completed counters to match\n";
        return 1;
    }

    if (m.wait_histogram.sample_count != task_count || m.exec_histogram.sample_count != task_count) {
        std::cerr << "expected histogram sample counts to match task count\n";
        return 1;
    }

    pool.shutdown();
    return 0;
}

