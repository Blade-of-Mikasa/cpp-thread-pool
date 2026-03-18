# cpp-thread-pool

一个基于 C++17 的线程池（单例）示例，保留核心能力：动态扩缩容、有界队列背压、`std::future` 回传，以及多种满队列处理策略。

## 特性
- 动态扩缩容：`min_threads ~ max_threads`，空闲超过 `idle_timeout` 后自动缩容
- 任务提交：`post()`、`submit()`、`try_submit()`、`submit_for()`
- 队列背压：`queue_capacity` 控制排队上限，`0` 表示无界队列
- 拒绝策略：`block`、`discard`、`throw_exception`、`caller_runs`
- 关闭语义：支持 `shutdown(drain)` 与 `shutdown(cancel)`
- 轻量状态查询：`size()`、`busy_size()`、`pending_size()`、`min_size()`、`max_size()`、`capacity()`、`queue_full_policy()`

## 快速开始
```cpp
#include "thread_pool.h"
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

int main() {
    thread_pool::options opts;
    opts.min_threads = 2;
    opts.max_threads = 8;
    opts.idle_timeout = 800ms;
    opts.queue_capacity = 256;
    opts.on_queue_full = thread_pool::reject_policy::block;

    auto &pool = thread_pool::instance(opts);

    auto f = pool.submit([](int x) { return x * 2; }, 21);
    std::cout << f.get() << '\n';

    pool.shutdown();
    return 0;
}
```

## 接口概览
- `thread_pool::instance(options)`：初始化/获取线程池单例
- `thread_pool::instance(min_threads, max_threads, idle_timeout)`：便捷初始化
- `thread_pool::instance(n)`：固定大小线程池
- `thread_pool::instance()`：获取已初始化单例；未初始化时抛出 `not_initialized`
- `post(f, args...)`：提交 fire-and-forget 任务
- `submit(f, args...)`：提交任务并返回 `std::future<T>`
- `try_submit(f, args...)`：非阻塞提交；失败时返回 `std::nullopt`
- `submit_for(timeout, f, args...)`：限时等待队列空间；超时返回 `std::nullopt`
- `wait_idle()`：等待当前任务全部完成
- `shutdown(drain)`：停止接收新任务，继续执行队列中已有任务
- `shutdown(cancel)`：停止接收新任务，并取消尚未开始的任务

## 动态扩缩容策略
1. 初始化时仅创建 `min_threads` 个工作线程
2. 新任务进入队列后，若待处理任务多于空闲线程，则按需扩容至 `max_threads`
3. 工作线程空闲时使用 `wait_for` 等待任务
4. 超过 `idle_timeout` 且当前线程数大于 `min_threads` 时，允许空闲线程退出

## 构建与测试
```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## 安装与 `find_package`
```bash
cmake --install build --prefix install
```

CMake 消费示例：
```cmake
find_package(ThreadPool CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ThreadPool::thread_pool)
```

## 示例
- `examples/example.cpp`：基础使用示例
