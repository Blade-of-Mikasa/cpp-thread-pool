#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <condition_variable>

class thread_pool{
public:
    explicit thread_pool(int num_thread);
    ~thread_pool();

    template<class F, class ... Args>
    void enqueue(F && f, Args && ... args);
private:
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> tasks;
    std::mutex mtx;
    std::condition_variable condition;
    bool stop{false};

    // void init(int num_thread);
};

template<class F, class... Args>
void thread_pool::enqueue(F &&f, Args &&... args) {
    std::unique_lock<std::mutex> lock(mtx);
    tasks.emplace(std::bind(
        std::forward<F>(f),
        std::forward<Args>(args)... // 用 参数包展开 而非 折叠表达式
    ));
}