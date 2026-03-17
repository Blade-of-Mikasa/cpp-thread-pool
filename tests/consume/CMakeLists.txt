cmake_minimum_required(VERSION 3.10)
project(ThreadPoolConsume LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ThreadPool CONFIG REQUIRED)

add_executable(consume main.cpp)
target_link_libraries(consume PRIVATE ThreadPool::thread_pool)

