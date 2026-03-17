#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;

enum class impl_kind {
    serial,
    thread_pool,
};

enum class work_kind {
    noop,
    spin,
    sleep,
};

const char *to_string(impl_kind kind) {
    switch (kind) {
    case impl_kind::serial:
        return "serial";
    case impl_kind::thread_pool:
        return "thread_pool";
    }
    return "unknown";
}

const char *to_string(work_kind kind) {
    switch (kind) {
    case work_kind::noop:
        return "noop";
    case work_kind::spin:
        return "spin";
    case work_kind::sleep:
        return "sleep";
    }
    return "unknown";
}

thread_pool::reject_policy parse_policy(std::string_view s) {
    if (s == "block") {
        return thread_pool::reject_policy::block;
    }
    if (s == "discard") {
        return thread_pool::reject_policy::discard;
    }
    if (s == "throw") {
        return thread_pool::reject_policy::throw_exception;
    }
    if (s == "caller_runs") {
        return thread_pool::reject_policy::caller_runs;
    }
    throw std::runtime_error("unknown --policy (expected: block|discard|throw|caller_runs)");
}

work_kind parse_work(std::string_view s) {
    if (s == "noop") {
        return work_kind::noop;
    }
    if (s == "spin") {
        return work_kind::spin;
    }
    if (s == "sleep") {
        return work_kind::sleep;
    }
    throw std::runtime_error("unknown --work (expected: noop|spin|sleep)");
}

impl_kind parse_impl(std::string_view s) {
    if (s == "serial") {
        return impl_kind::serial;
    }
    if (s == "thread_pool") {
        return impl_kind::thread_pool;
    }
    throw std::runtime_error("unknown --impl (expected: serial|thread_pool)");
}

std::uint64_t parse_u64(const char *value) {
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("invalid integer argument");
    }
    char *end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == nullptr || *end != '\0') {
        throw std::runtime_error("invalid integer argument");
    }
    return static_cast<std::uint64_t>(parsed);
}

std::size_t parse_size(const char *value) {
    return static_cast<std::size_t>(parse_u64(value));
}

struct config {
    impl_kind impl{impl_kind::thread_pool};
    work_kind work{work_kind::spin};

    std::size_t tasks{200'000};
    std::size_t producers{4};
    std::uint64_t work_us{50};
    bool use_submit{false};

    // thread_pool config (used when impl=thread_pool)
    std::size_t min_threads{2};
    std::size_t max_threads{8};
    std::uint64_t idle_ms{500};
    std::size_t capacity{0}; // 0 = unbounded
    thread_pool::reject_policy policy{thread_pool::reject_policy::block};

    bool emit_pool_metrics{true};
};

struct timings {
    double submit_ms{0.0};
    double total_ms{0.0};
};

void do_work(work_kind work, std::uint64_t work_us) {
    switch (work) {
    case work_kind::noop:
        // Prevent the optimizer from removing the loop entirely (especially in serial mode).
        static thread_local volatile std::uint64_t sink = 0;
        sink += 1;
        return;
    case work_kind::spin: {
        const auto end = clock_type::now() + std::chrono::microseconds(work_us);
        while (clock_type::now() < end) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        return;
    }
    case work_kind::sleep:
        std::this_thread::sleep_for(std::chrono::microseconds(work_us));
        return;
    }
}

timings run_serial(const config &cfg) {
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < cfg.tasks; ++i) {
        do_work(cfg.work, cfg.work_us);
    }
    const auto end = clock_type::now();

    timings t;
    t.submit_ms = std::chrono::duration<double, std::milli>(end - start).count();
    t.total_ms = t.submit_ms;
    return t;
}

timings run_thread_pool(const config &cfg, thread_pool::metrics_snapshot *out_metrics) {
    thread_pool::options opts;
    opts.min_threads = cfg.min_threads;
    opts.max_threads = cfg.max_threads;
    opts.idle_timeout = std::chrono::milliseconds(cfg.idle_ms);
    opts.queue_capacity = cfg.capacity;
    opts.on_queue_full = cfg.policy;
    opts.enable_metrics = cfg.emit_pool_metrics;

    auto &pool = thread_pool::instance(opts);

    std::atomic<std::size_t> submitted{0};
    std::atomic<std::size_t> done_producers{0};

    const auto submit_start = clock_type::now();

    std::vector<std::thread> producers;
    producers.reserve(cfg.producers);
    for (std::size_t p = 0; p < cfg.producers; ++p) {
        producers.emplace_back([&]() {
            while (true) {
                const auto i = submitted.fetch_add(1, std::memory_order_relaxed);
                if (i >= cfg.tasks) {
                    break;
                }

                try {
                    if (cfg.use_submit) {
                        (void)pool.submit([&cfg]() {
                            do_work(cfg.work, cfg.work_us);
                        });
                    } else {
                        pool.post([&cfg]() {
                            do_work(cfg.work, cfg.work_us);
                        });
                    }
                } catch (...) {
                    // When policy=throw_exception and the queue is full, submissions can fail.
                    // This benchmark treats those as "rejections" and continues.
                }
            }
            done_producers.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (auto &t : producers) {
        t.join();
    }
    const auto submit_end = clock_type::now();

    // Drain all accepted work.
    pool.wait_idle();
    const auto total_end = clock_type::now();

    if (out_metrics != nullptr) {
        *out_metrics = pool.metrics();
    }

    pool.shutdown(thread_pool::shutdown_mode::drain);

    timings t;
    t.submit_ms = std::chrono::duration<double, std::milli>(submit_end - submit_start).count();
    t.total_ms = std::chrono::duration<double, std::milli>(total_end - submit_start).count();
    return t;
}

config parse_args(int argc, char **argv) {
    config cfg;

    const auto hw = std::max(1u, std::thread::hardware_concurrency());
    cfg.max_threads = static_cast<std::size_t>(hw);
    cfg.min_threads = std::min<std::size_t>(2, cfg.max_threads);

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (arg == "--impl") {
            cfg.impl = parse_impl(next("--impl"));
        } else if (arg == "--work") {
            cfg.work = parse_work(next("--work"));
        } else if (arg == "--tasks") {
            cfg.tasks = parse_size(next("--tasks"));
        } else if (arg == "--producers") {
            cfg.producers = std::max<std::size_t>(1, parse_size(next("--producers")));
        } else if (arg == "--work-us") {
            cfg.work_us = parse_u64(next("--work-us"));
        } else if (arg == "--use-submit") {
            cfg.use_submit = true;
        } else if (arg == "--min") {
            cfg.min_threads = std::max<std::size_t>(1, parse_size(next("--min")));
        } else if (arg == "--max") {
            cfg.max_threads = std::max<std::size_t>(1, parse_size(next("--max")));
        } else if (arg == "--idle-ms") {
            cfg.idle_ms = parse_u64(next("--idle-ms"));
        } else if (arg == "--capacity") {
            cfg.capacity = parse_size(next("--capacity"));
        } else if (arg == "--policy") {
            cfg.policy = parse_policy(next("--policy"));
        } else if (arg == "--no-metrics") {
            cfg.emit_pool_metrics = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout
                << "stress_bench (one scenario per process)\n\n"
                << "Common:\n"
                << "  --impl serial|thread_pool\n"
                << "  --work noop|spin|sleep\n"
                << "  --tasks N (default: 200000)\n"
                << "  --producers N (default: 4)\n"
                << "  --work-us N (default: 50)\n"
                << "  --use-submit (use submit() instead of post())\n\n"
                << "thread_pool options:\n"
                << "  --min N\n"
                << "  --max N\n"
                << "  --idle-ms N\n"
                << "  --capacity N (0 = unbounded)\n"
                << "  --policy block|discard|throw|caller_runs\n"
                << "  --no-metrics\n\n"
                << "Output:\n"
                << "  Prints a single CSV row to stdout (always).\n";
            std::exit(0);
        } else {
            throw std::runtime_error(std::string("unknown argument: ") + std::string(arg));
        }
    }

    if (cfg.min_threads > cfg.max_threads) {
        throw std::runtime_error("--min must be <= --max");
    }
    return cfg;
}

void print_csv_header() {
    std::cout
        << "impl,mode,tasks,producers,work,work_us,min_threads,max_threads,capacity,policy,api,submit_ms,total_ms,throughput_per_sec,"
           "avg_wait_us,avg_exec_us,peak_threads,peak_pending_tasks,submitted_total,completed_total,rejected_total,canceled_total\n";
}

void print_csv_row(const config &cfg, const timings &t, const std::optional<thread_pool::metrics_snapshot> &m) {
    const auto tasks_done = m ? m->completed_total : cfg.tasks;
    const auto throughput = t.total_ms > 0.0 ? (static_cast<double>(tasks_done) / (t.total_ms / 1000.0)) : 0.0;

    auto policy = std::string("n/a");
    if (cfg.impl == impl_kind::thread_pool) {
        switch (cfg.policy) {
        case thread_pool::reject_policy::block:
            policy = "block";
            break;
        case thread_pool::reject_policy::discard:
            policy = "discard";
            break;
        case thread_pool::reject_policy::throw_exception:
            policy = "throw";
            break;
        case thread_pool::reject_policy::caller_runs:
            policy = "caller_runs";
            break;
        }
    }

    const auto mode = (cfg.impl == impl_kind::thread_pool && cfg.min_threads == cfg.max_threads) ? "fixed" : "dynamic";
    const auto api = (cfg.impl == impl_kind::thread_pool ? (cfg.use_submit ? "submit" : "post") : "n/a");

    std::cout << to_string(cfg.impl) << ',' << mode << ',' << cfg.tasks << ',' << cfg.producers << ',' << to_string(cfg.work)
              << ',' << cfg.work_us << ',' << cfg.min_threads << ',' << cfg.max_threads << ',' << cfg.capacity << ','
              << policy << ',' << api << ',' << t.submit_ms << ',' << t.total_ms << ',' << throughput << ',';

    if (m) {
        std::cout << m->avg_wait_us << ',' << m->avg_exec_us << ',' << m->peak_threads << ',' << m->peak_pending_tasks
                  << ',' << m->submitted_total << ',' << m->completed_total << ',' << m->rejected_total << ','
                  << m->canceled_total;
    } else {
        std::cout << "0,0,0,0,0,0,0,0";
    }

    std::cout << '\n';
}
}

int main(int argc, char **argv) {
    try {
        const auto cfg = parse_args(argc, argv);

        timings t;
        std::optional<thread_pool::metrics_snapshot> metrics;
        if (cfg.impl == impl_kind::serial) {
            t = run_serial(cfg);
        } else {
            thread_pool::metrics_snapshot m;
            t = run_thread_pool(cfg, cfg.emit_pool_metrics ? &m : nullptr);
            if (cfg.emit_pool_metrics) {
                metrics = m;
            }
        }

        print_csv_header();
        print_csv_row(cfg, t, metrics);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 2;
    }
}
