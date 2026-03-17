#include "thread_pool.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <atomic>
#include <random>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    thread_pool::options opts;
    opts.min_threads = 2;
    opts.max_threads = 8;
    opts.idle_timeout = 500ms;
    opts.queue_capacity = 128;
    opts.on_queue_full = thread_pool::reject_policy::block;
    opts.enable_metrics = true;

    auto &pool = thread_pool::instance(opts);

    // Time-series metrics output (CSV).
    std::ofstream stats_out("metrics_time_series.csv");
    stats_out
        << "uptime_ms,threads,busy_threads,pending_tasks,submitted_total,completed_total,throughput_per_sec,avg_wait_us,"
           "avg_exec_us,peak_threads,peak_pending_tasks\n";

    // Submit tasks in bursts to showcase scale up/down and backpressure behavior.
    std::atomic<bool> producer_done{false};
    constexpr std::uint64_t total_tasks = 5ull * 120ull;

    std::thread producer([&pool, &producer_done]() {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> work_ms(10, 60);

        for (int burst = 0; burst < 5; ++burst) {
            for (int i = 0; i < 120; ++i) {
                (void)pool.submit([delay = work_ms(rng)]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                });
            }
            std::this_thread::sleep_for(200ms);
        }

        producer_done.store(true);
    });

    // Poll metrics while the producer is running and the pool is working.
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto m = pool.metrics();
        stats_out << (std::chrono::duration<double, std::milli>(m.uptime).count()) << ',' << m.threads << ','
                  << m.busy_threads << ',' << m.pending_tasks << ',' << m.submitted_total << ',' << m.completed_total
                  << ',' << m.throughput_per_sec << ',' << m.avg_wait_us << ',' << m.avg_exec_us << ','
                  << m.peak_threads << ',' << m.peak_pending_tasks << '\n';

        if (producer_done.load() && m.completed_total >= total_tasks) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    if (producer.joinable()) {
        producer.join();
    }

    pool.wait_idle();

    // Snapshot + histograms.
    pool.write_stats_csv("metrics_stats.csv");
    pool.write_wait_histogram_csv("metrics_wait_histogram.csv");
    pool.write_exec_histogram_csv("metrics_exec_histogram.csv");

    std::cout << "Wrote:\n"
              << " - metrics_time_series.csv\n"
              << " - metrics_stats.csv\n"
              << " - metrics_wait_histogram.csv\n"
              << " - metrics_exec_histogram.csv\n";

    pool.shutdown();
    return 0;
}
