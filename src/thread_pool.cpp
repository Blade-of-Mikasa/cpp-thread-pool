#include "thread_pool.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace {
// 保底：线程数至少为 1，避免创建“0 线程”的无效线程池。
std::size_t normalize_thread_count(std::size_t count) {
    return std::max<std::size_t>(1, count);
}

// 保底：max >= min 且两者都 >= 1。
std::size_t normalize_max_thread_count(std::size_t min_threadnum, std::size_t max_threadnum) {
    return std::max(normalize_thread_count(min_threadnum), normalize_thread_count(max_threadnum));
}

std::chrono::milliseconds normalize_idle_timeout(std::chrono::milliseconds timeout) {
    return timeout.count() > 0 ? timeout : std::chrono::milliseconds(1000);
}

std::atomic<thread_pool *> &singleton_ptr() {
    static std::atomic<thread_pool *> ptr{nullptr};
    return ptr;
}

void add_duration(std::atomic<std::uint64_t> &target, std::chrono::steady_clock::duration duration) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    if (ns > 0) {
        target.fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
    }
}

}

thread_pool &thread_pool::instance() {
    auto *ptr = singleton_ptr().load(std::memory_order_acquire);
    if (ptr == nullptr) {
        throw not_initialized("thread_pool singleton has not been initialized; call instance(config) first");
    }
    return *ptr;
}

thread_pool &thread_pool::instance(std::size_t num_thread) {
    options opts;
    opts.min_threads = num_thread;
    opts.max_threads = num_thread;
    return instance(opts);
}

thread_pool &thread_pool::instance(std::size_t min_threadnum,
                                   std::size_t max_threadnum,
                                   std::chrono::milliseconds idle_timeout) {
    options opts;
    opts.min_threads = min_threadnum;
    opts.max_threads = max_threadnum;
    opts.idle_timeout = idle_timeout;
    return instance(opts);
}

thread_pool &thread_pool::instance(options opts) {
    static thread_pool pool(opts);
    singleton_ptr().store(&pool, std::memory_order_release);

    const auto expected_min = normalize_thread_count(opts.min_threads);
    const auto expected_max = normalize_max_thread_count(opts.min_threads, opts.max_threads);
    const auto expected_idle_timeout = normalize_idle_timeout(opts.idle_timeout);
    const auto expected_capacity = opts.queue_capacity;
    const auto expected_policy = opts.on_queue_full;

    if (pool.min_threadnum != expected_min ||
        pool.max_threadnum != expected_max ||
        pool.idle_timeout != expected_idle_timeout ||
        pool.queue_capacity != expected_capacity ||
        pool.on_queue_full != expected_policy) {
        throw std::runtime_error("thread_pool singleton already initialized with different configuration");
    }

    return pool;
}

thread_pool::thread_pool(options opts)
    : min_threadnum(normalize_thread_count(opts.min_threads)),
      max_threadnum(normalize_max_thread_count(opts.min_threads, opts.max_threads)),
      idle_timeout(normalize_idle_timeout(opts.idle_timeout)),
      queue_capacity(opts.queue_capacity),
      on_queue_full(opts.on_queue_full) {
    std::unique_lock<std::mutex> lock(mtx);
    threads.reserve(this->max_threadnum);
    for (std::size_t i = 0; i < this->min_threadnum; ++i) {
        add_thread_unlocked();
    }
}

thread_pool::~thread_pool() {
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop = true;
    }

    // 唤醒所有工作线程，让它们看到 stop=true 并在队列清空后退出。
    condition.notify_all();

    for (auto &worker_slot: threads) {
        if (worker_slot.worker.joinable()) {
            worker_slot.worker.join();
        }
    }
}

void thread_pool::add_thread_unlocked() {
    const auto started_at = std::chrono::steady_clock::now();
    auto finished_signal = std::make_shared<std::promise<void>>();

    worker_slot slot;
    slot.finished = finished_signal->get_future().share();
    slot.worker = std::thread([this, finished_signal]() {
        worker_loop(finished_signal);
    });

    threads.emplace_back(std::move(slot));
    ++all_threadnum;
    threads_created_count.fetch_add(1, std::memory_order_relaxed);
    add_duration(thread_create_time_ns_total, std::chrono::steady_clock::now() - started_at);
}

void thread_pool::cleanup_finished_threads_unlocked() {
    const auto immediately = std::chrono::milliseconds(0);
    finished_cleanup_pass_count.fetch_add(1, std::memory_order_relaxed);

    // 说明：
    // - worker 自己不会从 threads 容器里“删除自己”，而是通过 promise/future 通知已结束。
    // - enqueue 时在持锁情况下把已结束线程 join 掉并移除，保持 threads 容器整洁。
    auto new_end = std::remove_if(threads.begin(), threads.end(), [this, immediately](worker_slot &slot) {
        if (slot.finished.valid() && slot.finished.wait_for(immediately) == std::future_status::ready) {
            if (slot.worker.joinable()) {
                const auto join_started_at = std::chrono::steady_clock::now();
                slot.worker.join();
                finished_threads_joined_count.fetch_add(1, std::memory_order_relaxed);
                add_duration(finished_join_time_ns_total, std::chrono::steady_clock::now() - join_started_at);
            }
            return true;
        }
        return false;
    });

    threads.erase(new_end, threads.end());
}

void thread_pool::enqueue_task_unlocked(task_item task) {
    tasks.emplace(std::move(task));

    // 线程扩容策略：如果“待处理任务数 > 空闲线程数”，则尝试补线程到 max。
    const auto current_threads = all_threadnum.load();
    const auto current_busy_threads = busy_threadnum.load();
    std::size_t idle_threads = current_threads > current_busy_threads
                                   ? current_threads - current_busy_threads
                                   : 0;

    while (tasks.size() > idle_threads && all_threadnum.load() < max_threadnum) {
        add_thread_unlocked();
        ++idle_threads;
    }
}

void thread_pool::worker_loop(std::shared_ptr<std::promise<void>> finished_signal) {
    // 线程退出时：
    // - notify_all：让其他 worker 尽快从 wait_for 醒来重新评估条件
    // - set_value：供 enqueue/cleanup 检测该线程已结束，从而 join + erase
    auto finish_worker = [&]() {
        condition.notify_all();
        finished_signal->set_value();
    };

    while (true) {
        task_item task;
        bool should_exit = false;
        bool has_task = false;

        {
            std::unique_lock<std::mutex> lock(mtx);
            // 通过 wait_for 实现“空闲超时”：超时且无任务时可以按需缩容。
            const bool has_work = condition.wait_for(lock, idle_timeout, [this]() {
                return stop || !tasks.empty();
            });

            // stop 策略：不再接收新任务，但会把队列里已有任务处理完。
            if (stop && tasks.empty()) {
                --all_threadnum;
                worker_exit_stop_count.fetch_add(1, std::memory_order_relaxed);
                should_exit = true;
            } else if (!tasks.empty()) {
                task = std::move(tasks.front());
                tasks.pop();
                ++busy_threadnum;
                has_task = true;
                // 释放一个队列容量，唤醒可能在等待 capacity 的提交者。
                condition.notify_all();
            } else if (!has_work && all_threadnum.load() > min_threadnum) {
                // 空闲超时且当前线程数大于下限：允许该线程退出以缩容。
                --all_threadnum;
                worker_exit_idle_count.fetch_add(1, std::memory_order_relaxed);
                should_exit = true;
            }
        }

        if (should_exit) {
            finish_worker();
            return;
        }

        // 理论上：!stop 且 tasks.empty() 时会继续 wait_for；这里仅做防御性处理。
        if (!has_task) {
            continue;
        }
        execute_task_item(task);

        --busy_threadnum;
        condition.notify_all();
    }
}

void thread_pool::execute_task_item(task_item &task) {
    try {
        task.run();
    } catch (...) {
    }
}


void thread_pool::cancel_pending_tasks_unlocked() {
    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop();
        try {
            task.cancel();
        } catch (...) {
        }
    }
}

void thread_pool::shutdown(shutdown_mode mode) {
    {
        std::unique_lock<std::mutex> lock(mtx);
        if (stop) {
            return;
        }
        stop = true;
        if (mode == shutdown_mode::cancel) {
            cancel_pending_tasks_unlocked();
        }
    }

    condition.notify_all();
}

void thread_pool::wait_idle() {
    std::unique_lock<std::mutex> lock(mtx);
    condition.wait(lock, [this]() {
        return tasks.empty() && busy_threadnum.load() == 0;
    });
}

thread_pool::diagnostics_snapshot thread_pool::diagnostics() const {
    diagnostics_snapshot snapshot;
    snapshot.threads_created = threads_created_count.load(std::memory_order_relaxed);
    snapshot.worker_exit_idle = worker_exit_idle_count.load(std::memory_order_relaxed);
    snapshot.worker_exit_stop = worker_exit_stop_count.load(std::memory_order_relaxed);
    snapshot.finished_cleanup_passes = finished_cleanup_pass_count.load(std::memory_order_relaxed);
    snapshot.finished_threads_joined = finished_threads_joined_count.load(std::memory_order_relaxed);
    snapshot.thread_create_time_ns = thread_create_time_ns_total.load(std::memory_order_relaxed);
    snapshot.finished_join_time_ns = finished_join_time_ns_total.load(std::memory_order_relaxed);
    return snapshot;
}

std::size_t thread_pool::size() const {
    return all_threadnum.load();
}

std::size_t thread_pool::busy_size() const {
    return busy_threadnum.load();
}

std::size_t thread_pool::pending_size() const {
    std::lock_guard<std::mutex> lock(mtx);
    return tasks.size();
}

std::size_t thread_pool::min_size() const {
    return min_threadnum;
}

std::size_t thread_pool::max_size() const {
    return max_threadnum;
}

std::size_t thread_pool::capacity() const {
    return queue_capacity;
}

thread_pool::reject_policy thread_pool::queue_full_policy() const {
    return on_queue_full;
}
