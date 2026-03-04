#include "thread_pool.h"
#include <iostream>

int main() {
    // std::cout << "test begin!" << std::endl;
    thread_pool pool(5);
    for (int i = 1; i < 20; ++ i) {
        // std::cout << "enqueue begin" << std::endl;
        pool.enqueue([&](const int x) {
            std::cout << x << std::endl;
        }, i);
        // std::cout <<  "enqueue end" << std::endl;
    }
}