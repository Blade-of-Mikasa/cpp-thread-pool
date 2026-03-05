#include "thread_pool.h"
#include <iostream>

int main() {
    thread_pool pool(5);
    for (int i = 1; i < 20; ++ i) {
        pool.enqueue([&](const int x) {
            std::cout << x << std::endl;
        }, i);
    }
}