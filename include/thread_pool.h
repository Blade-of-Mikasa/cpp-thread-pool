#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <condition_variable>

class thread_pool {
public:
    explicit thread_pool(int num_thread);

    // 单例模式 禁止多个对象
    // thread_pool(const thread_pool &other) = delete;
    // thread_pool operator=(const thread_pool other) = delete;
    // thread_pool(thread_pool &&other) = delete;
    // thread_pool &operator=(thread_pool &&other) = delete;

    ~thread_pool();

    template<class F, class... Args>
    void enqueue(F &&f, Args &&... args);

    // 懒汉模式
    // static thread_pool& get_instance() {
    //     static thread_pool* ptr = nullptr;
    //     if (ptr == nullptr) ptr = new thread_pool;
    //     return *ptr;
    // }

    [[nodiscard]]size_t size() const;

private:
    std::vector<std::thread> threads;
    std::queue<std::function<void()> > tasks;
    std::mutex mtx;
    std::condition_variable condition;
    bool stop{false};
};

template<class F, class... Args>
void thread_pool::enqueue(F &&f, Args &&... args) {
    std::unique_lock<std::mutex> lock(mtx);
    tasks.emplace(std::bind(
        std::forward<F>(f),
        std::forward<Args>(args)... // 用 参数包展开 而非 折叠表达式
    ));
}
