cmake_minimum_required(VERSION 3.10)
project(ThreadPool)

set(CMAKE_CXX_STANDARD 17)

add_library(thread_pool
    src/thread_pool.cpp
)

target_include_directories(thread_pool PUBLIC include)

add_executable(example examples/example.cpp)
target_link_libraries(example thread_pool)