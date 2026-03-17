cmake_minimum_required(VERSION 3.10)
project(ThreadPool VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(THREAD_POOL_ENABLE_TIDY "Enable clang-tidy during builds" OFF)
option(THREAD_POOL_ENABLE_SANITIZERS "Enable ASAN/UBSAN (Clang/GCC)" OFF)

if(THREAD_POOL_ENABLE_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(WARNING "THREAD_POOL_ENABLE_TIDY=ON but clang-tidy was not found")
    endif()
endif()

if(THREAD_POOL_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        set(SANITIZER_COMPILE_FLAGS
            "-fsanitize=address,undefined"
            "-fno-omit-frame-pointer"
        )
        add_compile_options(${SANITIZER_COMPILE_FLAGS})

        set(SANITIZER_LINK_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_LINK_FLAGS}")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${SANITIZER_LINK_FLAGS}")
    else()
        message(WARNING "THREAD_POOL_ENABLE_SANITIZERS is only supported on Clang/GCC")
    endif()
endif()

add_library(thread_pool
    src/thread_pool.cpp
)

target_compile_features(thread_pool PUBLIC cxx_std_17)
target_include_directories(thread_pool
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS thread_pool
    EXPORT ThreadPoolTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ThreadPoolTargets
    FILE ThreadPoolTargets.cmake
    NAMESPACE ThreadPool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThreadPool
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThreadPoolConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThreadPool
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ThreadPoolConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThreadPool
)

add_executable(example examples/example.cpp)
target_link_libraries(example thread_pool)

add_executable(metrics_demo examples/metrics_demo.cpp)
target_link_libraries(metrics_demo thread_pool)

add_executable(stress_bench benchmarks/stress_bench.cpp)
target_link_libraries(stress_bench thread_pool)

enable_testing()

add_executable(test_thread_pool tests/test_thread_pool.cpp)
target_link_libraries(test_thread_pool thread_pool)

add_test(NAME thread_pool_dynamic COMMAND test_thread_pool)

add_executable(test_submit_future tests/test_submit_future.cpp)
target_link_libraries(test_submit_future thread_pool)
add_test(NAME thread_pool_submit_future COMMAND test_submit_future)

add_executable(test_try_submit_full tests/test_try_submit_full.cpp)
target_link_libraries(test_try_submit_full thread_pool)
add_test(NAME thread_pool_try_submit_full COMMAND test_try_submit_full)

add_executable(test_submit_for_timeout tests/test_submit_for_timeout.cpp)
target_link_libraries(test_submit_for_timeout thread_pool)
add_test(NAME thread_pool_submit_for_timeout COMMAND test_submit_for_timeout)

add_executable(test_reject_throw tests/test_reject_throw.cpp)
target_link_libraries(test_reject_throw thread_pool)
add_test(NAME thread_pool_reject_throw COMMAND test_reject_throw)

add_executable(test_reject_caller_runs tests/test_reject_caller_runs.cpp)
target_link_libraries(test_reject_caller_runs thread_pool)
add_test(NAME thread_pool_reject_caller_runs COMMAND test_reject_caller_runs)

add_executable(test_shutdown_cancel tests/test_shutdown_cancel.cpp)
target_link_libraries(test_shutdown_cancel thread_pool)
add_test(NAME thread_pool_shutdown_cancel COMMAND test_shutdown_cancel)

add_executable(test_metrics_snapshot tests/test_metrics_snapshot.cpp)
target_link_libraries(test_metrics_snapshot thread_pool)
add_test(NAME thread_pool_metrics_snapshot COMMAND test_metrics_snapshot)

add_executable(test_stress_shutdown_interleaving tests/test_stress_shutdown_interleaving.cpp)
target_link_libraries(test_stress_shutdown_interleaving thread_pool)
add_test(NAME thread_pool_stress_shutdown_interleaving COMMAND test_stress_shutdown_interleaving)
