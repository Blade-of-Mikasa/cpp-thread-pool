cmake_minimum_required(VERSION 3.10)
project(ThreadPool VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(THREAD_POOL_ENABLE_TIDY "Enable clang-tidy during builds" OFF)
option(THREAD_POOL_ENABLE_SANITIZERS "Enable ASAN/UBSAN (Clang/GCC)" OFF)

set(THREAD_POOL_SOURCES
    src/thread_pool.cpp
)

function(thread_pool_configure_target target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_17)
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
endfunction()

if(THREAD_POOL_ENABLE_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(WARNING "THREAD_POOL_ENABLE_TIDY=ON but clang-tidy was not found")
    endif()
endif()

add_library(thread_pool STATIC ${THREAD_POOL_SOURCES})
thread_pool_configure_target(thread_pool)

set(THREAD_POOL_DEV_LIBRARY thread_pool)

if(THREAD_POOL_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_library(thread_pool_sanitizers INTERFACE)
        target_compile_options(thread_pool_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_libraries(thread_pool_sanitizers INTERFACE
            -fsanitize=address,undefined
        )

        add_library(thread_pool_sanitized STATIC ${THREAD_POOL_SOURCES})
        thread_pool_configure_target(thread_pool_sanitized)
        target_link_libraries(thread_pool_sanitized PUBLIC thread_pool_sanitizers)

        set(THREAD_POOL_DEV_LIBRARY thread_pool_sanitized)
    else()
        message(WARNING "THREAD_POOL_ENABLE_SANITIZERS is only supported on Clang/GCC")
    endif()
endif()

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
target_link_libraries(example PRIVATE ${THREAD_POOL_DEV_LIBRARY})

add_executable(thread_pool_benchmark benchmarks/thread_pool_benchmark.cpp)
target_link_libraries(thread_pool_benchmark PRIVATE ${THREAD_POOL_DEV_LIBRARY})
if(WIN32)
    target_link_libraries(thread_pool_benchmark psapi)
endif()

enable_testing()

add_executable(test_thread_pool tests/test_thread_pool.cpp)
target_link_libraries(test_thread_pool PRIVATE ${THREAD_POOL_DEV_LIBRARY})

add_test(NAME thread_pool_dynamic COMMAND test_thread_pool)

add_executable(test_submit_future tests/test_submit_future.cpp)
target_link_libraries(test_submit_future PRIVATE ${THREAD_POOL_DEV_LIBRARY})
add_test(NAME thread_pool_submit_future COMMAND test_submit_future)

add_executable(test_try_submit_full tests/test_try_submit_full.cpp)
target_link_libraries(test_try_submit_full PRIVATE ${THREAD_POOL_DEV_LIBRARY})
add_test(NAME thread_pool_try_submit_full COMMAND test_try_submit_full)

add_executable(test_submit_for_timeout tests/test_submit_for_timeout.cpp)
target_link_libraries(test_submit_for_timeout PRIVATE ${THREAD_POOL_DEV_LIBRARY})
add_test(NAME thread_pool_submit_for_timeout COMMAND test_submit_for_timeout)

add_executable(test_reject_throw tests/test_reject_throw.cpp)
target_link_libraries(test_reject_throw PRIVATE ${THREAD_POOL_DEV_LIBRARY})
add_test(NAME thread_pool_reject_throw COMMAND test_reject_throw)

add_executable(test_reject_caller_runs tests/test_reject_caller_runs.cpp)
target_link_libraries(test_reject_caller_runs PRIVATE ${THREAD_POOL_DEV_LIBRARY})
add_test(NAME thread_pool_reject_caller_runs COMMAND test_reject_caller_runs)

add_executable(test_shutdown_cancel tests/test_shutdown_cancel.cpp)
target_link_libraries(test_shutdown_cancel PRIVATE ${THREAD_POOL_DEV_LIBRARY})
add_test(NAME thread_pool_shutdown_cancel COMMAND test_shutdown_cancel)

add_executable(test_stress_shutdown_interleaving tests/test_stress_shutdown_interleaving.cpp)
target_link_libraries(test_stress_shutdown_interleaving PRIVATE ${THREAD_POOL_DEV_LIBRARY})
add_test(NAME thread_pool_stress_shutdown_interleaving COMMAND test_stress_shutdown_interleaving)
