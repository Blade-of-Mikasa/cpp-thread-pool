#include "thread_pool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#define POPEN _popen
#define PCLOSE _pclose
#else
#include <sys/resource.h>
#define POPEN popen
#define PCLOSE pclose
#endif

namespace {
using clock_type = std::chrono::steady_clock;

std::size_t hardware_threads() {
    const auto value = std::thread::hardware_concurrency();
    return value == 0 ? 4u : static_cast<std::size_t>(value);
}

enum class run_mode {
    compare,
    fixed,
    dynamic,
};

enum class work_kind {
    noop,
    spin,
    sleep,
};

enum class load_profile {
    random,
    steady,
};

struct config {
    run_mode mode{run_mode::compare};
    load_profile profile{load_profile::random};
    std::size_t tasks{240'000};
    std::size_t producers{std::max<std::size_t>(4, hardware_threads())};
    work_kind work{work_kind::spin};
    std::uint64_t work_us{40};
    std::uint64_t work_us_min{5};
    std::uint64_t work_us_max{120};
    std::uint64_t phase_gap_us_max{2'500};
    std::size_t phases{24};
    std::uint64_t seed{20260318};
    std::size_t min_threads{2};
    std::size_t max_threads{std::max<std::size_t>(4, hardware_threads())};
    std::size_t fixed_threads{0};
    std::size_t queue_capacity{0};
    std::chrono::milliseconds idle_timeout{150};
    std::chrono::milliseconds sample_interval{5};
    int repeats{3};
};

struct workload_phase {
    std::size_t active_producers{1};
    std::size_t tasks{0};
    std::uint64_t work_us{0};
    std::uint64_t gap_us{0};
};

struct workload_summary {
    std::size_t phases{0};
    double avg_active_producers{0.0};
    std::size_t max_active_producers{0};
    std::size_t max_phase_tasks{0};
    std::uint64_t max_work_us{0};
    std::uint64_t max_gap_us{0};
};

struct pool_samples {
    std::uint64_t sample_count{0};
    double avg_threads{0.0};
    double avg_busy_threads{0.0};
    double avg_pending_tasks{0.0};
    std::size_t peak_threads{0};
    std::size_t peak_busy_threads{0};
    std::size_t peak_pending_tasks{0};
};

struct process_snapshot {
    std::uint64_t user_cpu_ns{0};
    std::uint64_t kernel_cpu_ns{0};
    std::uint64_t peak_working_set_bytes{0};
    std::uint64_t peak_pagefile_bytes{0};
    std::uint64_t handle_count{0};
};

struct benchmark_result {
    std::string mode;
    std::string profile;
    double submit_ms{0.0};
    double total_ms{0.0};
    double throughput_per_sec{0.0};
    std::size_t tasks{0};
    std::size_t producers{0};
    std::size_t threads{0};
    workload_summary workload;
    pool_samples samples;
    process_snapshot process{};
    thread_pool::diagnostics_snapshot diagnostics{};
};

class reusable_barrier {
public:
    explicit reusable_barrier(std::size_t participants)
        : participants_(participants), remaining_(participants) {
    }

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto generation = generation_;
        if (--remaining_ == 0) {
            ++generation_;
            remaining_ = participants_;
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [&]() {
            return generation_ != generation;
        });
    }

private:
    std::size_t participants_;
    std::size_t remaining_;
    std::size_t generation_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
};

std::size_t parse_size(std::string_view value, const char *name) {
    try {
        return static_cast<std::size_t>(std::stoull(std::string(value)));
    } catch (...) {
        throw std::runtime_error(std::string("invalid value for ") + name + ": " + std::string(value));
    }
}

std::uint64_t parse_u64(std::string_view value, const char *name) {
    try {
        return static_cast<std::uint64_t>(std::stoull(std::string(value)));
    } catch (...) {
        throw std::runtime_error(std::string("invalid value for ") + name + ": " + std::string(value));
    }
}

int parse_int(std::string_view value, const char *name) {
    try {
        return std::stoi(std::string(value));
    } catch (...) {
        throw std::runtime_error(std::string("invalid value for ") + name + ": " + std::string(value));
    }
}

run_mode parse_mode(std::string_view value) {
    if (value == "compare") {
        return run_mode::compare;
    }
    if (value == "fixed") {
        return run_mode::fixed;
    }
    if (value == "dynamic") {
        return run_mode::dynamic;
    }
    throw std::runtime_error("unknown --mode, expected compare|fixed|dynamic");
}

work_kind parse_work(std::string_view value) {
    if (value == "noop") {
        return work_kind::noop;
    }
    if (value == "spin") {
        return work_kind::spin;
    }
    if (value == "sleep") {
        return work_kind::sleep;
    }
    throw std::runtime_error("unknown --work, expected noop|spin|sleep");
}

load_profile parse_profile(std::string_view value) {
    if (value == "random") {
        return load_profile::random;
    }
    if (value == "steady") {
        return load_profile::steady;
    }
    throw std::runtime_error("unknown --profile, expected random|steady");
}

const char *to_string(run_mode mode) {
    switch (mode) {
    case run_mode::compare:
        return "compare";
    case run_mode::fixed:
        return "fixed";
    case run_mode::dynamic:
        return "dynamic";
    }
    return "unknown";
}

const char *to_string(work_kind work) {
    switch (work) {
    case work_kind::noop:
        return "noop";
    case work_kind::spin:
        return "spin";
    case work_kind::sleep:
        return "sleep";
    }
    return "unknown";
}

const char *to_string(load_profile profile) {
    switch (profile) {
    case load_profile::random:
        return "random";
    case load_profile::steady:
        return "steady";
    }
    return "unknown";
}

void print_usage(const char *argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Compare dynamic and fixed thread-pool behavior under changing concurrency.\n\n"
        << "Options:\n"
        << "  --mode compare|fixed|dynamic   Run compare (default) or a single mode\n"
        << "  --profile random|steady        Workload profile (default: random)\n"
        << "  --tasks N                      Total tasks (default: 240000)\n"
        << "  --producers N                  Producer thread count (default: hardware-based)\n"
        << "  --work noop|spin|sleep         Task work type (default: spin)\n"
        << "  --work-us N                    Steady profile task duration in us (default: 40)\n"
        << "  --work-us-min N                Random profile min task duration in us (default: 5)\n"
        << "  --work-us-max N                Random profile max task duration in us (default: 120)\n"
        << "  --gap-us-max N                 Random profile max gap between phases in us (default: 2500)\n"
        << "  --phases N                     Random profile phase count (default: 24)\n"
        << "  --seed N                       Random profile seed (default: 20260318)\n"
        << "  --min N                        Dynamic mode min threads (default: 2)\n"
        << "  --max N                        Dynamic mode max threads (default: hardware-based)\n"
        << "  --fixed-threads N              Fixed mode thread count (default: use --max)\n"
        << "  --capacity N                   Queue capacity, 0 = unbounded (default: 0)\n"
        << "  --idle-ms N                    Dynamic idle timeout in ms (default: 150)\n"
        << "  --sample-ms N                  Runtime sample interval in ms (default: 5)\n"
        << "  --repeats N                    Compare mode repeats (default: 3)\n"
        << "  --help                         Show this help\n\n"
        << "Examples:\n"
        << "  " << argv0 << " --profile random --tasks 300000 --producers 16 --min 2 --max 8 --repeats 3\n"
        << "  " << argv0 << " --profile steady --mode fixed --tasks 300000 --producers 16 --fixed-threads 8\n";
}

void normalize_config(config &cfg) {
    cfg.tasks = std::max<std::size_t>(1, cfg.tasks);
    cfg.producers = std::max<std::size_t>(1, cfg.producers);
    cfg.min_threads = std::max<std::size_t>(1, cfg.min_threads);
    cfg.max_threads = std::max(cfg.min_threads, std::max<std::size_t>(1, cfg.max_threads));
    cfg.fixed_threads = cfg.fixed_threads == 0 ? cfg.max_threads : std::max<std::size_t>(1, cfg.fixed_threads);
    cfg.work_us = std::max<std::uint64_t>(1, cfg.work_us);
    cfg.work_us_min = std::max<std::uint64_t>(1, cfg.work_us_min);
    cfg.work_us_max = std::max(cfg.work_us_min, cfg.work_us_max);
    cfg.phases = std::max<std::size_t>(1, std::min(cfg.tasks, cfg.phases));
    cfg.idle_timeout = std::max(std::chrono::milliseconds(1), cfg.idle_timeout);
    cfg.sample_interval = std::max(std::chrono::milliseconds(1), cfg.sample_interval);
    cfg.repeats = std::max(1, cfg.repeats);
}

void do_work(work_kind work, std::uint64_t work_us) {
    switch (work) {
    case work_kind::noop:
        return;
    case work_kind::spin: {
        const auto deadline = clock_type::now() + std::chrono::microseconds(work_us);
        while (clock_type::now() < deadline) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        return;
    }
    case work_kind::sleep:
        std::this_thread::sleep_for(std::chrono::microseconds(work_us));
        return;
    }
}

std::string quote_shell_arg(const std::string &value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (char ch: value) {
        if (ch == '"') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

std::vector<workload_phase> build_workload(const config &cfg) {
    if (cfg.profile == load_profile::steady) {
        return {workload_phase{cfg.producers, cfg.tasks, cfg.work_us, 0}};
    }

    std::mt19937_64 rng(cfg.seed);
    std::uniform_int_distribution<std::uint64_t> weight_dist(1, 100);
    std::uniform_int_distribution<std::uint64_t> work_dist(cfg.work_us_min, cfg.work_us_max);
    std::uniform_int_distribution<std::uint64_t> gap_dist(0, cfg.phase_gap_us_max);
    std::uniform_int_distribution<std::size_t> producer_dist(1, cfg.producers);

    std::vector<std::uint64_t> weights(cfg.phases);
    for (auto &weight: weights) {
        weight = weight_dist(rng);
    }

    const auto total_weight = std::accumulate(weights.begin(), weights.end(), std::uint64_t{0});
    std::vector<workload_phase> phases(cfg.phases);
    std::size_t assigned_tasks = 0;
    for (std::size_t i = 0; i < cfg.phases; ++i) {
        std::size_t phase_tasks = static_cast<std::size_t>(cfg.tasks * weights[i] / total_weight);
        if (i + 1 == cfg.phases) {
            phase_tasks = cfg.tasks - assigned_tasks;
        }
        phases[i].tasks = phase_tasks;
        phases[i].active_producers = std::min<std::size_t>(producer_dist(rng), std::max<std::size_t>(1, phase_tasks));
        phases[i].work_us = work_dist(rng);
        phases[i].gap_us = gap_dist(rng);
        assigned_tasks += phase_tasks;
    }

    std::size_t remaining = cfg.tasks - assigned_tasks;
    for (std::size_t i = 0; remaining > 0; ++i, --remaining) {
        phases[i % phases.size()].tasks += 1;
    }

    phases.erase(
        std::remove_if(phases.begin(), phases.end(), [](const workload_phase &phase) {
            return phase.tasks == 0;
        }),
        phases.end());

    return phases;
}

workload_summary summarize_workload(const std::vector<workload_phase> &phases) {
    workload_summary summary;
    summary.phases = phases.size();
    if (phases.empty()) {
        return summary;
    }

    double total_active = 0.0;
    for (const auto &phase: phases) {
        total_active += static_cast<double>(phase.active_producers);
        summary.max_active_producers = std::max(summary.max_active_producers, phase.active_producers);
        summary.max_phase_tasks = std::max(summary.max_phase_tasks, phase.tasks);
        summary.max_work_us = std::max(summary.max_work_us, phase.work_us);
        summary.max_gap_us = std::max(summary.max_gap_us, phase.gap_us);
    }
    summary.avg_active_producers = total_active / static_cast<double>(phases.size());
    return summary;
}

thread_pool::diagnostics_snapshot subtract_diagnostics(const thread_pool::diagnostics_snapshot &after,
                                                       const thread_pool::diagnostics_snapshot &before) {
    thread_pool::diagnostics_snapshot delta;
    delta.threads_created = after.threads_created - before.threads_created;
    delta.worker_exit_idle = after.worker_exit_idle - before.worker_exit_idle;
    delta.worker_exit_stop = after.worker_exit_stop - before.worker_exit_stop;
    delta.finished_cleanup_passes = after.finished_cleanup_passes - before.finished_cleanup_passes;
    delta.finished_threads_joined = after.finished_threads_joined - before.finished_threads_joined;
    delta.thread_create_time_ns = after.thread_create_time_ns - before.thread_create_time_ns;
    delta.finished_join_time_ns = after.finished_join_time_ns - before.finished_join_time_ns;
    return delta;
}

void add_pool_sample(pool_samples &samples, std::size_t threads, std::size_t busy, std::size_t pending) {
    ++samples.sample_count;
    const auto n = static_cast<double>(samples.sample_count);
    samples.avg_threads += (static_cast<double>(threads) - samples.avg_threads) / n;
    samples.avg_busy_threads += (static_cast<double>(busy) - samples.avg_busy_threads) / n;
    samples.avg_pending_tasks += (static_cast<double>(pending) - samples.avg_pending_tasks) / n;
    samples.peak_threads = std::max(samples.peak_threads, threads);
    samples.peak_busy_threads = std::max(samples.peak_busy_threads, busy);
    samples.peak_pending_tasks = std::max(samples.peak_pending_tasks, pending);
}

#ifdef _WIN32
std::uint64_t filetime_to_ns(const FILETIME &value) {
    ULARGE_INTEGER raw{};
    raw.LowPart = value.dwLowDateTime;
    raw.HighPart = value.dwHighDateTime;
    return raw.QuadPart * 100;
}
#endif

process_snapshot capture_process_snapshot() {
    process_snapshot snapshot;
#ifdef _WIN32
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0) {
        snapshot.kernel_cpu_ns = filetime_to_ns(kernel);
        snapshot.user_cpu_ns = filetime_to_ns(user);
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)) != 0) {
        snapshot.peak_working_set_bytes = static_cast<std::uint64_t>(memory.PeakWorkingSetSize);
        snapshot.peak_pagefile_bytes = static_cast<std::uint64_t>(memory.PeakPagefileUsage);
    }

    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles) != 0) {
        snapshot.handle_count = static_cast<std::uint64_t>(handles);
    }
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        snapshot.user_cpu_ns = static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1'000'000'000ull +
                               static_cast<std::uint64_t>(usage.ru_utime.tv_usec) * 1'000ull;
        snapshot.kernel_cpu_ns = static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1'000'000'000ull +
                                 static_cast<std::uint64_t>(usage.ru_stime.tv_usec) * 1'000ull;
        snapshot.peak_working_set_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ull;
    }
#endif
    return snapshot;
}

process_snapshot diff_process_snapshot(const process_snapshot &after, const process_snapshot &before) {
    process_snapshot delta;
    delta.user_cpu_ns = after.user_cpu_ns - before.user_cpu_ns;
    delta.kernel_cpu_ns = after.kernel_cpu_ns - before.kernel_cpu_ns;
    delta.peak_working_set_bytes = after.peak_working_set_bytes;
    delta.peak_pagefile_bytes = after.peak_pagefile_bytes;
    delta.handle_count = after.handle_count;
    return delta;
}

benchmark_result parse_result_line(const std::string &output) {
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.rfind("RESULT ", 0) != 0) {
            continue;
        }

        benchmark_result result;
        std::istringstream line_stream(line.substr(7));
        std::string token;
        while (line_stream >> token) {
            const auto pos = token.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            const auto key = token.substr(0, pos);
            const auto value = token.substr(pos + 1);

            if (key == "mode") {
                result.mode = value;
            } else if (key == "profile") {
                result.profile = value;
            } else if (key == "submit_ms") {
                result.submit_ms = std::stod(value);
            } else if (key == "total_ms") {
                result.total_ms = std::stod(value);
            } else if (key == "throughput") {
                result.throughput_per_sec = std::stod(value);
            } else if (key == "tasks") {
                result.tasks = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "producers") {
                result.producers = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "threads") {
                result.threads = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "phases") {
                result.workload.phases = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "avg_active") {
                result.workload.avg_active_producers = std::stod(value);
            } else if (key == "max_active") {
                result.workload.max_active_producers = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "max_phase_tasks") {
                result.workload.max_phase_tasks = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "max_work_us") {
                result.workload.max_work_us = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "max_gap_us") {
                result.workload.max_gap_us = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "avg_pool_threads") {
                result.samples.avg_threads = std::stod(value);
            } else if (key == "avg_busy_threads") {
                result.samples.avg_busy_threads = std::stod(value);
            } else if (key == "avg_pending") {
                result.samples.avg_pending_tasks = std::stod(value);
            } else if (key == "peak_pool_threads") {
                result.samples.peak_threads = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "peak_busy_threads") {
                result.samples.peak_busy_threads = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "peak_pending") {
                result.samples.peak_pending_tasks = static_cast<std::size_t>(std::stoull(value));
            } else if (key == "user_cpu_ms") {
                result.process.user_cpu_ns = static_cast<std::uint64_t>(std::llround(std::stod(value) * 1'000'000.0));
            } else if (key == "kernel_cpu_ms") {
                result.process.kernel_cpu_ns = static_cast<std::uint64_t>(std::llround(std::stod(value) * 1'000'000.0));
            } else if (key == "peak_ws_kb") {
                result.process.peak_working_set_bytes = static_cast<std::uint64_t>(std::stoull(value)) * 1024ull;
            } else if (key == "peak_pf_kb") {
                result.process.peak_pagefile_bytes = static_cast<std::uint64_t>(std::stoull(value)) * 1024ull;
            } else if (key == "handles") {
                result.process.handle_count = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "diag_created") {
                result.diagnostics.threads_created = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "diag_idle_exit") {
                result.diagnostics.worker_exit_idle = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "diag_stop_exit") {
                result.diagnostics.worker_exit_stop = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "diag_cleanup_pass") {
                result.diagnostics.finished_cleanup_passes = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "diag_joined") {
                result.diagnostics.finished_threads_joined = static_cast<std::uint64_t>(std::stoull(value));
            } else if (key == "diag_create_ms") {
                result.diagnostics.thread_create_time_ns =
                    static_cast<std::uint64_t>(std::llround(std::stod(value) * 1'000'000.0));
            } else if (key == "diag_join_ms") {
                result.diagnostics.finished_join_time_ns =
                    static_cast<std::uint64_t>(std::llround(std::stod(value) * 1'000'000.0));
            }
        }

        if (!result.mode.empty()) {
            return result;
        }
    }

    throw std::runtime_error("failed to parse child benchmark output:\n" + output);
}

benchmark_result run_single(const config &cfg, run_mode mode) {
    thread_pool::options opts;
    opts.queue_capacity = cfg.queue_capacity;
    opts.on_queue_full = thread_pool::reject_policy::block;
    opts.idle_timeout = cfg.idle_timeout;

    std::size_t threads = cfg.fixed_threads;
    if (mode == run_mode::dynamic) {
        opts.min_threads = cfg.min_threads;
        opts.max_threads = cfg.max_threads;
        threads = cfg.max_threads;
    } else {
        opts.min_threads = cfg.fixed_threads;
        opts.max_threads = cfg.fixed_threads;
    }

    auto &pool = thread_pool::instance(opts);
    const auto diagnostics_before = pool.diagnostics();
    const auto process_before = capture_process_snapshot();

    const auto phases = build_workload(cfg);
    const auto workload = summarize_workload(phases);

    std::mutex start_mutex;
    std::condition_variable ready_cv;
    std::condition_variable start_cv;
    std::size_t ready_count = 0;
    bool start = false;
    reusable_barrier phase_barrier(cfg.producers);

    std::atomic<bool> started{false};
    std::atomic<bool> sampling{true};
    pool_samples samples;
    std::mutex samples_mutex;

    std::thread sampler([&]() {
        while (!started.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        while (sampling.load(std::memory_order_acquire)) {
            const auto threads_now = pool.size();
            const auto busy_now = pool.busy_size();
            const auto pending_now = pool.pending_size();
            {
                std::lock_guard<std::mutex> lock(samples_mutex);
                add_pool_sample(samples, threads_now, busy_now, pending_now);
            }
            std::this_thread::sleep_for(cfg.sample_interval);
        }

        const auto threads_now = pool.size();
        const auto busy_now = pool.busy_size();
        const auto pending_now = pool.pending_size();
        std::lock_guard<std::mutex> lock(samples_mutex);
        add_pool_sample(samples, threads_now, busy_now, pending_now);
    });

    std::vector<std::thread> producers;
    producers.reserve(cfg.producers);
    for (std::size_t producer_index = 0; producer_index < cfg.producers; ++producer_index) {
        producers.emplace_back([&, producer_index]() {
            {
                std::unique_lock<std::mutex> lock(start_mutex);
                ++ready_count;
                ready_cv.notify_one();
                start_cv.wait(lock, [&]() {
                    return start;
                });
            }

            for (std::size_t phase_index = 0; phase_index < phases.size(); ++phase_index) {
                const auto &phase = phases[phase_index];
                phase_barrier.arrive_and_wait();

                if (producer_index < phase.active_producers) {
                    const auto base = phase.tasks / phase.active_producers;
                    const auto extra = phase.tasks % phase.active_producers;
                    const auto local_tasks = base + (producer_index < extra ? 1 : 0);
                    for (std::size_t i = 0; i < local_tasks; ++i) {
                        pool.post([work = cfg.work, work_us = phase.work_us]() {
                            do_work(work, work_us);
                        });
                    }
                }

                phase_barrier.arrive_and_wait();
                if (producer_index == 0 && phase.gap_us > 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(phase.gap_us));
                }
                phase_barrier.arrive_and_wait();
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(start_mutex);
        ready_cv.wait(lock, [&]() {
            return ready_count == cfg.producers;
        });
    }

    const auto started_at = clock_type::now();
    {
        std::lock_guard<std::mutex> lock(start_mutex);
        start = true;
    }
    started.store(true, std::memory_order_release);
    start_cv.notify_all();

    for (auto &producer: producers) {
        producer.join();
    }
    const auto submit_done_at = clock_type::now();

    pool.wait_idle();
    const auto finished_at = clock_type::now();
    sampling.store(false, std::memory_order_release);
    sampler.join();

    const auto diagnostics_after = pool.diagnostics();
    const auto process_after = capture_process_snapshot();
    pool.shutdown();

    benchmark_result result;
    result.mode = to_string(mode);
    result.profile = to_string(cfg.profile);
    result.submit_ms = std::chrono::duration<double, std::milli>(submit_done_at - started_at).count();
    result.total_ms = std::chrono::duration<double, std::milli>(finished_at - started_at).count();
    const auto total_sec = std::chrono::duration<double>(finished_at - started_at).count();
    result.throughput_per_sec = total_sec > 0.0 ? static_cast<double>(cfg.tasks) / total_sec : 0.0;
    result.tasks = cfg.tasks;
    result.producers = cfg.producers;
    result.threads = threads;
    result.workload = workload;
    {
        std::lock_guard<std::mutex> lock(samples_mutex);
        result.samples = samples;
    }
    result.process = diff_process_snapshot(process_after, process_before);
    result.diagnostics = subtract_diagnostics(diagnostics_after, diagnostics_before);
    return result;
}

benchmark_result run_child_process(const std::filesystem::path &exe_path, const config &cfg, run_mode mode) {
    std::ostringstream command;
    command << quote_shell_arg(exe_path.string())
            << " --mode " << to_string(mode)
            << " --profile " << to_string(cfg.profile)
            << " --tasks " << cfg.tasks
            << " --producers " << cfg.producers
            << " --work " << to_string(cfg.work)
            << " --work-us " << cfg.work_us
            << " --work-us-min " << cfg.work_us_min
            << " --work-us-max " << cfg.work_us_max
            << " --gap-us-max " << cfg.phase_gap_us_max
            << " --phases " << cfg.phases
            << " --seed " << cfg.seed
            << " --min " << cfg.min_threads
            << " --max " << cfg.max_threads
            << " --fixed-threads " << cfg.fixed_threads
            << " --capacity " << cfg.queue_capacity
            << " --idle-ms " << cfg.idle_timeout.count()
            << " --sample-ms " << cfg.sample_interval.count()
            << " 2>&1";

    FILE *pipe = POPEN(command.str().c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to start child benchmark process");
    }

    std::string output;
    char buffer[256];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }

    const auto exit_code = PCLOSE(pipe);
    if (exit_code != 0) {
        throw std::runtime_error("child benchmark process failed:\n" + output);
    }

    return parse_result_line(output);
}

double average_total_ms(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += result.total_ms;
    }
    return total / static_cast<double>(results.size());
}

double average_throughput(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += result.throughput_per_sec;
    }
    return total / static_cast<double>(results.size());
}

double average_peak_ws_kb(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += static_cast<double>(result.process.peak_working_set_bytes) / 1024.0;
    }
    return total / static_cast<double>(results.size());
}

double average_user_cpu_ms(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += static_cast<double>(result.process.user_cpu_ns) / 1'000'000.0;
    }
    return total / static_cast<double>(results.size());
}

double average_kernel_cpu_ms(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += static_cast<double>(result.process.kernel_cpu_ns) / 1'000'000.0;
    }
    return total / static_cast<double>(results.size());
}

double average_avg_threads(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += result.samples.avg_threads;
    }
    return total / static_cast<double>(results.size());
}

double average_peak_pending(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += static_cast<double>(result.samples.peak_pending_tasks);
    }
    return total / static_cast<double>(results.size());
}

double average_create_ms(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += static_cast<double>(result.diagnostics.thread_create_time_ns) / 1'000'000.0;
    }
    return total / static_cast<double>(results.size());
}

double average_join_ms(const std::vector<benchmark_result> &results) {
    double total = 0.0;
    for (const auto &result: results) {
        total += static_cast<double>(result.diagnostics.finished_join_time_ns) / 1'000'000.0;
    }
    return total / static_cast<double>(results.size());
}

std::uint64_t average_created_threads(const std::vector<benchmark_result> &results) {
    std::uint64_t total = 0;
    for (const auto &result: results) {
        total += result.diagnostics.threads_created;
    }
    return total / static_cast<std::uint64_t>(results.size());
}

std::uint64_t average_idle_exits(const std::vector<benchmark_result> &results) {
    std::uint64_t total = 0;
    for (const auto &result: results) {
        total += result.diagnostics.worker_exit_idle;
    }
    return total / static_cast<std::uint64_t>(results.size());
}

} // namespace

int main(int argc, char **argv) {
    try {
        config cfg;

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            auto require_value = [&](const char *name) -> std::string_view {
                if (i + 1 >= argc) {
                    throw std::runtime_error(std::string("missing value for ") + name);
                }
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            }
            if (arg == "--mode") {
                cfg.mode = parse_mode(require_value("--mode"));
            } else if (arg == "--profile") {
                cfg.profile = parse_profile(require_value("--profile"));
            } else if (arg == "--tasks") {
                cfg.tasks = parse_size(require_value("--tasks"), "--tasks");
            } else if (arg == "--producers") {
                cfg.producers = parse_size(require_value("--producers"), "--producers");
            } else if (arg == "--work") {
                cfg.work = parse_work(require_value("--work"));
            } else if (arg == "--work-us") {
                cfg.work_us = parse_u64(require_value("--work-us"), "--work-us");
            } else if (arg == "--work-us-min") {
                cfg.work_us_min = parse_u64(require_value("--work-us-min"), "--work-us-min");
            } else if (arg == "--work-us-max") {
                cfg.work_us_max = parse_u64(require_value("--work-us-max"), "--work-us-max");
            } else if (arg == "--gap-us-max") {
                cfg.phase_gap_us_max = parse_u64(require_value("--gap-us-max"), "--gap-us-max");
            } else if (arg == "--phases") {
                cfg.phases = parse_size(require_value("--phases"), "--phases");
            } else if (arg == "--seed") {
                cfg.seed = parse_u64(require_value("--seed"), "--seed");
            } else if (arg == "--min") {
                cfg.min_threads = parse_size(require_value("--min"), "--min");
            } else if (arg == "--max") {
                cfg.max_threads = parse_size(require_value("--max"), "--max");
            } else if (arg == "--fixed-threads") {
                cfg.fixed_threads = parse_size(require_value("--fixed-threads"), "--fixed-threads");
            } else if (arg == "--capacity") {
                cfg.queue_capacity = parse_size(require_value("--capacity"), "--capacity");
            } else if (arg == "--idle-ms") {
                cfg.idle_timeout = std::chrono::milliseconds(parse_int(require_value("--idle-ms"), "--idle-ms"));
            } else if (arg == "--sample-ms") {
                cfg.sample_interval = std::chrono::milliseconds(parse_int(require_value("--sample-ms"), "--sample-ms"));
            } else if (arg == "--repeats") {
                cfg.repeats = parse_int(require_value("--repeats"), "--repeats");
            } else {
                throw std::runtime_error("unknown argument: " + std::string(arg));
            }
        }

        normalize_config(cfg);

        if (cfg.mode == run_mode::fixed || cfg.mode == run_mode::dynamic) {
            const auto result = run_single(cfg, cfg.mode);
            std::cout << std::fixed << std::setprecision(3)
                      << "RESULT"
                      << " mode=" << result.mode
                      << " profile=" << result.profile
                      << " submit_ms=" << result.submit_ms
                      << " total_ms=" << result.total_ms
                      << " throughput=" << result.throughput_per_sec
                      << " tasks=" << result.tasks
                      << " producers=" << result.producers
                      << " threads=" << result.threads
                      << " phases=" << result.workload.phases
                      << " avg_active=" << result.workload.avg_active_producers
                      << " max_active=" << result.workload.max_active_producers
                      << " max_phase_tasks=" << result.workload.max_phase_tasks
                      << " max_work_us=" << result.workload.max_work_us
                      << " max_gap_us=" << result.workload.max_gap_us
                      << " avg_pool_threads=" << result.samples.avg_threads
                      << " avg_busy_threads=" << result.samples.avg_busy_threads
                      << " avg_pending=" << result.samples.avg_pending_tasks
                      << " peak_pool_threads=" << result.samples.peak_threads
                      << " peak_busy_threads=" << result.samples.peak_busy_threads
                      << " peak_pending=" << result.samples.peak_pending_tasks
                      << " user_cpu_ms=" << (static_cast<double>(result.process.user_cpu_ns) / 1'000'000.0)
                      << " kernel_cpu_ms=" << (static_cast<double>(result.process.kernel_cpu_ns) / 1'000'000.0)
                      << " peak_ws_kb=" << (result.process.peak_working_set_bytes / 1024ull)
                      << " peak_pf_kb=" << (result.process.peak_pagefile_bytes / 1024ull)
                      << " handles=" << result.process.handle_count
                      << " diag_created=" << result.diagnostics.threads_created
                      << " diag_idle_exit=" << result.diagnostics.worker_exit_idle
                      << " diag_stop_exit=" << result.diagnostics.worker_exit_stop
                      << " diag_cleanup_pass=" << result.diagnostics.finished_cleanup_passes
                      << " diag_joined=" << result.diagnostics.finished_threads_joined
                      << " diag_create_ms=" << (static_cast<double>(result.diagnostics.thread_create_time_ns) / 1'000'000.0)
                      << " diag_join_ms=" << (static_cast<double>(result.diagnostics.finished_join_time_ns) / 1'000'000.0)
                      << '\n';
            return 0;
        }

        const auto exe_path = std::filesystem::absolute(argv[0]);
        std::vector<benchmark_result> fixed_results;
        std::vector<benchmark_result> dynamic_results;
        fixed_results.reserve(static_cast<std::size_t>(cfg.repeats));
        dynamic_results.reserve(static_cast<std::size_t>(cfg.repeats));

        for (int round = 0; round < cfg.repeats; ++round) {
            const auto round_seed = cfg.seed + static_cast<std::uint64_t>(round);
            auto round_cfg = cfg;
            round_cfg.seed = round_seed;
            const auto fixed = run_child_process(exe_path, round_cfg, run_mode::fixed);
            const auto dynamic = run_child_process(exe_path, round_cfg, run_mode::dynamic);
            fixed_results.push_back(fixed);
            dynamic_results.push_back(dynamic);

            std::cout << std::fixed << std::setprecision(3)
                      << "round " << (round + 1) << '/' << cfg.repeats
                      << " seed=" << round_seed
                      << ": fixed total=" << fixed.total_ms << " ms, dynamic total=" << dynamic.total_ms << " ms"
                      << ", fixed peak_ws=" << (fixed.process.peak_working_set_bytes / 1024ull) << " KB"
                      << ", dynamic peak_ws=" << (dynamic.process.peak_working_set_bytes / 1024ull) << " KB\n";
        }

        const auto fixed_avg_ms = average_total_ms(fixed_results);
        const auto dynamic_avg_ms = average_total_ms(dynamic_results);
        const auto fixed_avg_throughput = average_throughput(fixed_results);
        const auto dynamic_avg_throughput = average_throughput(dynamic_results);
        const auto fixed_avg_threads = average_avg_threads(fixed_results);
        const auto dynamic_avg_threads = average_avg_threads(dynamic_results);
        const auto fixed_avg_peak_pending = average_peak_pending(fixed_results);
        const auto dynamic_avg_peak_pending = average_peak_pending(dynamic_results);
        const auto fixed_avg_peak_ws = average_peak_ws_kb(fixed_results);
        const auto dynamic_avg_peak_ws = average_peak_ws_kb(dynamic_results);
        const auto fixed_avg_user_cpu = average_user_cpu_ms(fixed_results);
        const auto dynamic_avg_user_cpu = average_user_cpu_ms(dynamic_results);
        const auto fixed_avg_kernel_cpu = average_kernel_cpu_ms(fixed_results);
        const auto dynamic_avg_kernel_cpu = average_kernel_cpu_ms(dynamic_results);
        const auto fixed_create_ms = average_create_ms(fixed_results);
        const auto dynamic_create_ms = average_create_ms(dynamic_results);
        const auto fixed_join_ms = average_join_ms(fixed_results);
        const auto dynamic_join_ms = average_join_ms(dynamic_results);
        const auto dynamic_created_threads = average_created_threads(dynamic_results);
        const auto dynamic_idle_exits = average_idle_exits(dynamic_results);

        std::cout << std::fixed << std::setprecision(3)
                  << "\ncompare summary\n"
                  << "  profile                 : " << to_string(cfg.profile) << '\n'
                  << "  tasks                   : " << cfg.tasks << '\n'
                  << "  producers               : " << cfg.producers << '\n'
                  << "  work                    : " << to_string(cfg.work) << '\n'
                  << "  dynamic threads         : " << cfg.min_threads << " -> " << cfg.max_threads << '\n'
                  << "  fixed threads           : " << cfg.fixed_threads << '\n'
                  << "  repeats                 : " << cfg.repeats << '\n'
                  << "  random phases           : " << dynamic_results.front().workload.phases << '\n'
                  << "  avg active producers    : " << dynamic_results.front().workload.avg_active_producers << '\n'
                  << "  max active producers    : " << dynamic_results.front().workload.max_active_producers << '\n'
                  << "  max phase tasks         : " << dynamic_results.front().workload.max_phase_tasks << '\n'
                  << "  max work us             : " << dynamic_results.front().workload.max_work_us << '\n'
                  << "  max gap us              : " << dynamic_results.front().workload.max_gap_us << '\n'
                  << "  fixed avg total         : " << fixed_avg_ms << " ms\n"
                  << "  dynamic avg total       : " << dynamic_avg_ms << " ms\n"
                  << "  fixed throughput        : " << fixed_avg_throughput << " tasks/s\n"
                  << "  dynamic throughput      : " << dynamic_avg_throughput << " tasks/s\n"
                  << "  fixed avg pool threads  : " << fixed_avg_threads << '\n'
                  << "  dynamic avg pool threads: " << dynamic_avg_threads << '\n'
                  << "  fixed avg peak pending  : " << fixed_avg_peak_pending << '\n'
                  << "  dynamic avg peak pending: " << dynamic_avg_peak_pending << '\n'
                  << "  fixed user/kernel cpu   : " << fixed_avg_user_cpu << " / " << fixed_avg_kernel_cpu << " ms\n"
                  << "  dynamic user/kernel cpu : " << dynamic_avg_user_cpu << " / " << dynamic_avg_kernel_cpu << " ms\n"
                  << "  fixed peak working set  : " << fixed_avg_peak_ws << " KB\n"
                  << "  dynamic peak working set: " << dynamic_avg_peak_ws << " KB\n"
                  << "  dynamic runtime created : " << dynamic_created_threads << " threads\n"
                  << "  dynamic idle exits      : " << dynamic_idle_exits << " threads\n"
                  << "  fixed create/join cost  : " << fixed_create_ms << " / " << fixed_join_ms << " ms\n"
                  << "  dynamic create/join cost: " << dynamic_create_ms << " / " << dynamic_join_ms << " ms\n";

        if (dynamic_avg_ms < fixed_avg_ms) {
            const auto delta_ms = fixed_avg_ms - dynamic_avg_ms;
            const auto delta_pct = fixed_avg_ms > 0.0 ? delta_ms / fixed_avg_ms * 100.0 : 0.0;
            std::cout << "  winner                  : dynamic faster by " << delta_ms << " ms (" << delta_pct << "%)\n";
        } else if (fixed_avg_ms < dynamic_avg_ms) {
            const auto delta_ms = dynamic_avg_ms - fixed_avg_ms;
            const auto delta_pct = dynamic_avg_ms > 0.0 ? delta_ms / dynamic_avg_ms * 100.0 : 0.0;
            std::cout << "  winner                  : fixed faster by " << delta_ms << " ms (" << delta_pct << "%)\n";
        } else {
            std::cout << "  winner                  : tie\n";
        }

        const auto extra_create_ms = std::max(0.0, dynamic_create_ms - fixed_create_ms);
        const auto extra_join_ms = std::max(0.0, dynamic_join_ms - fixed_join_ms);
        if (extra_create_ms >= extra_join_ms && extra_create_ms > 0.0) {
            std::cout << "  biggest measured overhead: runtime thread creation (" << extra_create_ms << " ms)\n";
        } else if (extra_join_ms > 0.0) {
            std::cout << "  biggest measured overhead: finished-thread join cleanup (" << extra_join_ms << " ms)\n";
        } else {
            std::cout << "  biggest measured overhead: no measurable scaling-management cost in this run\n";
        }

        if (dynamic_avg_threads < fixed_avg_threads) {
            const auto saved_threads = fixed_avg_threads - dynamic_avg_threads;
            std::cout << "  dynamic value           : average live threads reduced by " << saved_threads << '\n';
        }
        if (dynamic_avg_peak_ws < fixed_avg_peak_ws) {
            const auto saved_kb = fixed_avg_peak_ws - dynamic_avg_peak_ws;
            std::cout << "  dynamic value           : peak working set reduced by " << saved_kb << " KB\n";
        }

        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
