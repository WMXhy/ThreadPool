cmake_minimum_required(VERSION 3.13)
project(ThreadPool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

add_executable(main ./src/main.cpp)
target_link_libraries(main PRIVATE Threads::Threads)

set(THREADPOOL_SANITIZER "none" CACHE STRING "none, asan-ubsan, or tsan")
set_property(CACHE THREADPOOL_SANITIZER PROPERTY STRINGS none asan-ubsan tsan)

if(THREADPOOL_SANITIZER STREQUAL "asan-ubsan")
    set(sanitizer_flag "-fsanitize=address,undefined")
elseif(THREADPOOL_SANITIZER STREQUAL "tsan")
    set(sanitizer_flag "-fsanitize=thread")
elseif(NOT THREADPOOL_SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unknown THREADPOOL_SANITIZER: ${THREADPOOL_SANITIZER}")
endif()

if(DEFINED sanitizer_flag)
    target_compile_options(main PRIVATE
        "$<$<CONFIG:Debug>:${sanitizer_flag};-g;-fno-omit-frame-pointer>"
    )
    target_link_options(main PRIVATE
        "$<$<CONFIG:Debug>:${sanitizer_flag}>"
    )
endif()