#include "thread_pool.h"

#include <chrono>

int main() {
    using namespace std::chrono_literals;

    thread_pool::options opts;
    opts.min_threads = 1;
    opts.max_threads = 1;
    opts.idle_timeout = 100ms;
    opts.queue_capacity = 8;
    opts.on_queue_full = thread_pool::reject_policy::block;

    auto &pool = thread_pool::instance(opts);
    auto f = pool.submit([] { return 42; });
    if (f.get() != 42) {
        return 1;
    }

    pool.shutdown();
    return 0;
}

