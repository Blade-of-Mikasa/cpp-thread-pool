#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// 一个简单的 C++17 线程池实现（单例模式）。
//
// 特性：
// - 支持固定大小：thread_pool::instance(n)。
// - 支持动态扩缩容：thread_pool::instance(min, max, idle_timeout)。
// - 支持有界队列 + 背压（capacity）与拒绝策略（reject_policy）。
// - 支持 submit()/try_submit()/submit_for()，submit 返回 future 并回传异常。
// - 任务队列为 FIFO（std::queue），多线程安全入队。
//
// 设计说明（重要）：
// - 线程池析构会停止接收新任务，并唤醒所有工作线程；工作线程会把队列里剩余任务处理完再退出。
// - enqueue 为兼容接口（内部调用 post，fire-and-forget）。
class thread_pool {
public:
    enum class shutdown_mode {
        // 不再接收新任务，但会把队列中已有任务执行完。
        drain,
        // 不再接收新任务，并取消队列中尚未开始的任务（对应 future 会收到取消异常）。
        cancel,
    };

    enum class reject_policy {
        // 队列满时阻塞等待（submit 会一直等；submit_for 最多等到超时；try_submit 不等待直接失败）。
        block,
        // 队列满时丢弃（submit 返回一个 future，但 future.get() 会抛出 task_rejected；try/for 返回空）。
        discard,
        // 队列满时直接抛出 task_rejected（不返回 future）。
        throw_exception,
        // 队列满时在调用者线程同步执行任务（不入队），并返回对应 future。
        caller_runs,
    };

    struct options {
        std::size_t min_threads{1};
        std::size_t max_threads{1};
        std::chrono::milliseconds idle_timeout{std::chrono::milliseconds(1000)};

        // 0 表示无界队列；>0 表示最大排队任务数（不包含正在执行的任务）。
        std::size_t queue_capacity{0};
        reject_policy on_queue_full{reject_policy::block};

        // 为后续可观测性预留；本次实现中默认打开（开销：每个任务会记录时间戳）。
        bool enable_metrics{true};
    };

    class task_rejected final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class task_canceled final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class not_initialized final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // 单例访问入口：
    // - 第一次调用 instance(...) 会创建全局唯一实例。
    // - 后续可用无参 instance() 获取同一实例，避免重复传参。
    // - 如果后续用不同参数再次初始化，会抛出异常，避免“静默错配”。
    static thread_pool &instance();
    static thread_pool &instance(std::size_t num_thread);
    static thread_pool &instance(std::size_t min_threadnum,
                                 std::size_t max_threadnum,
                                 std::chrono::milliseconds idle_timeout = std::chrono::milliseconds(1000));
    static thread_pool &instance(options opts);

    thread_pool(const thread_pool &) = delete;
    thread_pool &operator=(const thread_pool &) = delete;
    thread_pool(thread_pool &&) = delete;
    thread_pool &operator=(thread_pool &&) = delete;

    // 单例对象会在进程退出时析构；析构将 join 所有工作线程。
    ~thread_pool();

    template<class F, class... Args>
    void enqueue(F &&f, Args &&... args);

    // Fire-and-forget 提交：不返回 future、不开辟 promise。
    // 适合高吞吐场景（避免大量 future/promise 分配开销）。
    template<class F, class... Args>
    void post(F &&f, Args &&... args);

    template<class F, class... Args>
    auto submit(F &&f, Args &&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

    template<class F, class... Args>
    auto try_submit(F &&f, Args &&... args)
        -> std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>>;

    template<class Rep, class Period, class F, class... Args>
    auto submit_for(std::chrono::duration<Rep, Period> timeout, F &&f, Args &&... args)
        -> std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>>;

    // 线程池控制：
    // - shutdown(drain) ：停止接收新任务，等待队列中的任务被取走执行。
    // - shutdown(cancel)：停止接收新任务，并取消队列中未开始任务。
    void shutdown(shutdown_mode mode = shutdown_mode::drain);

    // 等待“当前”任务全部完成（队列为空且 busy==0）。
    // 注意：这不是 shutdown；其他线程仍可继续 submit 新任务（除非已 shutdown）。
    void wait_idle();

    struct histogram_snapshot {
        // 每个 bucket 的计数快照；最后一个 bucket 表示“> 最大边界”。
        std::array<std::uint64_t, 22> buckets{};
        std::uint64_t sample_count{0};
        std::uint64_t total_ns{0};
    };

    struct metrics_snapshot {
        // 运行时长（从 instance() 初始化开始）。
        std::chrono::nanoseconds uptime{0};

        std::size_t threads{0};
        std::size_t busy_threads{0};
        std::size_t pending_tasks{0};

        std::size_t peak_threads{0};
        std::size_t peak_pending_tasks{0};

        std::uint64_t submitted_total{0};
        std::uint64_t started_total{0};
        std::uint64_t completed_total{0};
        std::uint64_t canceled_total{0};
        std::uint64_t rejected_total{0};

        double throughput_per_sec{0.0};
        double avg_wait_us{0.0};
        double avg_exec_us{0.0};

        histogram_snapshot wait_histogram;
        histogram_snapshot exec_histogram;
    };

    [[nodiscard]] metrics_snapshot metrics() const;
    void write_stats_csv(const std::string &path) const;
    void write_wait_histogram_csv(const std::string &path) const;
    void write_exec_histogram_csv(const std::string &path) const;

    // 观测接口：
    // - size()/busy_size() 是原子计数快照（不会加锁）；用于监控/统计，读数可能有轻微瞬态误差。
    // - pending_size() 会加锁读取队列大小。
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t busy_size() const;
    [[nodiscard]] std::size_t pending_size() const;
    [[nodiscard]] std::size_t min_size() const;
    [[nodiscard]] std::size_t max_size() const;
    [[nodiscard]] std::size_t capacity() const;
    [[nodiscard]] reject_policy queue_full_policy() const;

private:
    // 构造函数私有化：外部只能通过 instance() 获取唯一实例。
    explicit thread_pool(options opts);

    struct worker_slot {
        std::thread worker;
        // 用于标记该 worker 是否已经“自然退出”（缩容或 stop）。
        // enqueue 时会清理这些已结束的线程：join + 从 threads 容器中移除。
        std::shared_future<void> finished;
    };

    struct task_item {
        std::function<void()> run;
        std::function<void()> cancel;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    class histogram {
    public:
        void observe(std::chrono::nanoseconds value);
        [[nodiscard]] histogram_snapshot snapshot() const;

        static constexpr std::size_t bucket_count = 22;

        // bucket_count - 1 个上界；最后一个 bucket 为 “> 最大上界”。
        static constexpr std::array<std::chrono::nanoseconds, bucket_count - 1> upper_bounds{
            std::chrono::nanoseconds(1000),            // 1us
            std::chrono::nanoseconds(2000),            // 2us
            std::chrono::nanoseconds(5000),            // 5us
            std::chrono::nanoseconds(10'000),          // 10us
            std::chrono::nanoseconds(20'000),          // 20us
            std::chrono::nanoseconds(50'000),          // 50us
            std::chrono::nanoseconds(100'000),         // 100us
            std::chrono::nanoseconds(200'000),         // 200us
            std::chrono::nanoseconds(500'000),         // 500us
            std::chrono::nanoseconds(1'000'000),       // 1ms
            std::chrono::nanoseconds(2'000'000),       // 2ms
            std::chrono::nanoseconds(5'000'000),       // 5ms
            std::chrono::nanoseconds(10'000'000),      // 10ms
            std::chrono::nanoseconds(20'000'000),      // 20ms
            std::chrono::nanoseconds(50'000'000),      // 50ms
            std::chrono::nanoseconds(100'000'000),     // 100ms
            std::chrono::nanoseconds(200'000'000),     // 200ms
            std::chrono::nanoseconds(500'000'000),     // 500ms
            std::chrono::nanoseconds(1'000'000'000),   // 1s
            std::chrono::nanoseconds(2'000'000'000),   // 2s
            std::chrono::nanoseconds(5'000'000'000),   // 5s
        };

    private:
        std::array<std::atomic<std::uint64_t>, bucket_count> buckets{};
        std::atomic<std::uint64_t> sample_count{0};
        std::atomic<std::uint64_t> total_ns{0};
    };

    // 注意：以下 *_unlocked 方法要求调用方已经持有 mtx。
    void add_thread_unlocked();
    void cleanup_finished_threads_unlocked();
    void enqueue_task_unlocked(task_item task);
    void worker_loop(std::shared_ptr<std::promise<void>> finished_signal);
    void execute_task_item(task_item &task, std::chrono::nanoseconds wait_time);
    void cancel_pending_tasks_unlocked();

    std::size_t min_threadnum;
    std::size_t max_threadnum;
    std::chrono::milliseconds idle_timeout;
    std::size_t queue_capacity;
    reject_policy on_queue_full;
    bool enable_metrics;
    std::atomic<std::size_t> busy_threadnum{0};
    std::atomic<std::size_t> all_threadnum{0};
    std::vector<worker_slot> threads;
    std::queue<task_item> tasks;
    mutable std::mutex mtx;
    std::condition_variable condition;
    // stop 只在持有 mtx 时读写：
    // - 析构置 stop=true
    // - worker_loop 读取 stop 并决定是否退出
    bool stop{false};

    std::chrono::steady_clock::time_point created_at{std::chrono::steady_clock::now()};

    std::atomic<std::uint64_t> submitted_tasks{0};
    std::atomic<std::uint64_t> started_tasks{0};
    std::atomic<std::uint64_t> completed_tasks{0};
    std::atomic<std::uint64_t> canceled_tasks{0};
    std::atomic<std::uint64_t> rejected_tasks{0};
    std::atomic<std::size_t> peak_threadnum{0};
    std::atomic<std::size_t> peak_pending_tasks{0};

    histogram wait_time_histogram;
    histogram exec_time_histogram;
};

template<class F, class... Args>
void thread_pool::enqueue(F &&f, Args &&... args) {
    post(std::forward<F>(f), std::forward<Args>(args)...);
}

template<class F, class... Args>
void thread_pool::post(F &&f, Args &&... args) {
    task_item task;
    task.enqueued_at = enable_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    task.run = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    task.cancel = []() {
        // no-op
    };

    std::unique_lock<std::mutex> lock(mtx);
    if (stop) {
        throw std::runtime_error("post on stopped thread_pool");
    }

    cleanup_finished_threads_unlocked();

    if (queue_capacity > 0 && tasks.size() >= queue_capacity) {
        switch (on_queue_full) {
        case reject_policy::block: {
            if (all_threadnum.load() < max_threadnum) {
                add_thread_unlocked();
            }

            condition.wait(lock, [this]() {
                return stop || queue_capacity == 0 || tasks.size() < queue_capacity;
            });

            if (stop) {
                throw std::runtime_error("post on stopped thread_pool");
            }
            break;
        }
        case reject_policy::throw_exception:
            rejected_tasks.fetch_add(1, std::memory_order_relaxed);
            throw task_rejected("task queue full");
        case reject_policy::discard:
            rejected_tasks.fetch_add(1, std::memory_order_relaxed);
            return;
        case reject_policy::caller_runs: {
            submitted_tasks.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            const auto wait_time = std::chrono::nanoseconds(0);
            execute_task_item(task, wait_time);
            return;
        }
        }
    }

    enqueue_task_unlocked(std::move(task));

    lock.unlock();
    condition.notify_one();
}

template<class F, class... Args>
auto thread_pool::submit(F &&f, Args &&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    using return_type = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    auto promise = std::make_shared<std::promise<return_type>>();
    auto future = promise->get_future();

    auto bound = [func = std::decay_t<F>(std::forward<F>(f)),
                  args_tuple = std::make_tuple(std::decay_t<Args>(std::forward<Args>(args))...)]() mutable
        -> return_type {
        return std::apply(std::move(func), std::move(args_tuple));
    };

    task_item task;
    task.enqueued_at = enable_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    task.run = [promise, bound = std::move(bound)]() mutable {
        try {
            if constexpr (std::is_void_v<return_type>) {
                bound();
                promise->set_value();
            } else {
                promise->set_value(bound());
            }
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };
    task.cancel = [promise]() mutable {
        try {
            throw task_canceled("task canceled");
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };

    std::unique_lock<std::mutex> lock(mtx);
    if (stop) {
        throw std::runtime_error("submit on stopped thread_pool");
    }

    cleanup_finished_threads_unlocked();

    if (queue_capacity > 0 && tasks.size() >= queue_capacity) {
        switch (on_queue_full) {
        case reject_policy::block: {
            // 可扩容时先补线程，帮助更快消化队列。
            if (all_threadnum.load() < max_threadnum) {
                add_thread_unlocked();
            }

            condition.wait(lock, [this]() {
                return stop || queue_capacity == 0 || tasks.size() < queue_capacity;
            });

            if (stop) {
                throw std::runtime_error("submit on stopped thread_pool");
            }
            break;
        }
        case reject_policy::throw_exception:
            rejected_tasks.fetch_add(1, std::memory_order_relaxed);
            throw task_rejected("task queue full");
        case reject_policy::discard:
            rejected_tasks.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            try {
                throw task_rejected("task queue full");
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            return future;
        case reject_policy::caller_runs: {
            submitted_tasks.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            const auto wait_time = std::chrono::nanoseconds(0);
            execute_task_item(task, wait_time);
            return future;
        }
        }
    }

    enqueue_task_unlocked(std::move(task));

    lock.unlock();
    condition.notify_one();
    return future;
}

template<class F, class... Args>
auto thread_pool::try_submit(F &&f, Args &&... args)
    -> std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>> {
    using return_type = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    auto promise = std::make_shared<std::promise<return_type>>();
    auto future = promise->get_future();

    auto bound = [func = std::decay_t<F>(std::forward<F>(f)),
                  args_tuple = std::make_tuple(std::decay_t<Args>(std::forward<Args>(args))...)]() mutable
        -> return_type {
        return std::apply(std::move(func), std::move(args_tuple));
    };

    task_item task;
    task.enqueued_at = enable_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    task.run = [promise, bound = std::move(bound)]() mutable {
        try {
            if constexpr (std::is_void_v<return_type>) {
                bound();
                promise->set_value();
            } else {
                promise->set_value(bound());
            }
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };
    task.cancel = [promise]() mutable {
        try {
            throw task_canceled("task canceled");
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };

    std::unique_lock<std::mutex> lock(mtx);
    if (stop) {
        throw std::runtime_error("try_submit on stopped thread_pool");
    }

    cleanup_finished_threads_unlocked();

    if (queue_capacity > 0 && tasks.size() >= queue_capacity) {
        switch (on_queue_full) {
        case reject_policy::caller_runs: {
            submitted_tasks.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            const auto wait_time = std::chrono::nanoseconds(0);
            execute_task_item(task, wait_time);
            return future;
        }
        case reject_policy::throw_exception:
            rejected_tasks.fetch_add(1, std::memory_order_relaxed);
            throw task_rejected("task queue full");
        case reject_policy::block:
        case reject_policy::discard:
            rejected_tasks.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
    }

    enqueue_task_unlocked(std::move(task));

    lock.unlock();
    condition.notify_one();
    return future;
}

template<class Rep, class Period, class F, class... Args>
auto thread_pool::submit_for(std::chrono::duration<Rep, Period> timeout, F &&f, Args &&... args)
    -> std::optional<std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>> {
    using return_type = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    auto promise = std::make_shared<std::promise<return_type>>();
    auto future = promise->get_future();

    auto bound = [func = std::decay_t<F>(std::forward<F>(f)),
                  args_tuple = std::make_tuple(std::decay_t<Args>(std::forward<Args>(args))...)]() mutable
        -> return_type {
        return std::apply(std::move(func), std::move(args_tuple));
    };

    task_item task;
    task.enqueued_at = enable_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    task.run = [promise, bound = std::move(bound)]() mutable {
        try {
            if constexpr (std::is_void_v<return_type>) {
                bound();
                promise->set_value();
            } else {
                promise->set_value(bound());
            }
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };
    task.cancel = [promise]() mutable {
        try {
            throw task_canceled("task canceled");
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    };

    std::unique_lock<std::mutex> lock(mtx);
    if (stop) {
        throw std::runtime_error("submit_for on stopped thread_pool");
    }

    cleanup_finished_threads_unlocked();

    if (queue_capacity > 0 && tasks.size() >= queue_capacity) {
        switch (on_queue_full) {
        case reject_policy::caller_runs: {
            submitted_tasks.fetch_add(1, std::memory_order_relaxed);
            lock.unlock();
            const auto wait_time = std::chrono::nanoseconds(0);
            execute_task_item(task, wait_time);
            return future;
        }
        case reject_policy::throw_exception:
            rejected_tasks.fetch_add(1, std::memory_order_relaxed);
            throw task_rejected("task queue full");
        case reject_policy::discard:
            rejected_tasks.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        case reject_policy::block: {
            // 可扩容时先补线程，帮助更快消化队列。
            if (all_threadnum.load() < max_threadnum) {
                add_thread_unlocked();
            }

            const auto deadline = std::chrono::steady_clock::now() + timeout;
            const bool has_space = condition.wait_until(lock, deadline, [this]() {
                return stop || queue_capacity == 0 || tasks.size() < queue_capacity;
            });

            if (stop) {
                throw std::runtime_error("submit_for on stopped thread_pool");
            }
            if (!has_space) {
                rejected_tasks.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }
            break;
        }
        }
    }

    enqueue_task_unlocked(std::move(task));

    lock.unlock();
    condition.notify_one();
    return future;
}
