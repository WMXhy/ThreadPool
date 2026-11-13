#include "../include/ThreadPool.hpp"
#include <cassert>
#include <future>
#include <stdexcept>
#include <vector>

int main() {
    std::future<int> failed;
    std::vector<std::future<int>> results;

    {
        ThreadPool pool(1);

        failed = pool.submit([]() -> int {
            throw std::runtime_error("task failed");
        });

        for (int i = 0; i < 100; ++i) {
            results.push_back(pool.submit([i] { return i * i; }));
        }
    }

    bool caught = false;
    try {
        failed.get();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    assert(caught);

    for (int i = 0; i < 100; ++i) {
        assert(results[i].get() == i * i);
    }
}