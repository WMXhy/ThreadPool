# C++17 Thread Pool

[简体中文](README.md)

A fixed-size thread pool. The constructor starts a group of worker threads. Callers submit work with `submit()` and receive a `std::future`. Workers take jobs from a shared queue and run them outside the lock. When the pool is destroyed, it drains the queue and waits for all workers to exit.

- [`include/ThreadPool.hpp`](include/ThreadPool.hpp) contains the implementation.
- [`src/main.cpp`](src/main.cpp) demonstrates how to check task results, exception propagation, and destruction.
- [`ThreadPool.drawio.png`](ThreadPool.drawio.png) contains the thread pool design diagram.

## 1. Submitting a task

```cpp
#include "ThreadPool.hpp"
#include <iostream>

int main() {
    ThreadPool pool(2);

    auto result = pool.submit([](int a, int b) {
        return a + b;
    }, 3, 4);

    std::cout << result.get() << '\n';  // 7
}
```

`submit(f, args...)` accepts a callable and its arguments. A task with a value result gives the caller a `std::future<T>`, while a task returning `void` gives a `std::future<void>`. If the result is still pending, `get()` blocks the calling thread. Each `std::future` allows one call to `get()`.

## 2. Member variables and worker threads

| Member | Role |
| --- | --- |
| `workers` | Holds the `std::thread` objects created by the constructor so the destructor can join them. |
| `tasks` | Stores `std::function<void()>` objects in FIFO order. Tasks with different signatures share the same zero-argument interface. |
| `mtx` | Protects queue inspection, insertion, and removal. The same mutex is held when setting the shutdown flag or checking the wait predicate. |
| `cv` | Lets idle workers wait for new work or a shutdown notification. |
| `stopping` | Starts as `false` and is set to `true` during destruction. The flag is atomic, and checks involving the queue still take place under the mutex. |
| `threadSize` | Holds the fixed worker count. The caller can provide it explicitly. The default comes from `std::thread::hardware_concurrency()`. |

The constructor starts each thread with `workers.emplace_back([this] { work(); })`. Every worker runs the same `work()` loop. After completing a task, it returns to the queue to wait or pick up the next one.

## 3. How `submit()` queues a task

1. **Determine the result type.** The implementation computes `ReturnType` with `std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>` and returns a `std::future<ReturnType>`.
2. **Store the callable and arguments.** The outer zero-argument lambda captures the callable as `func` and puts the arguments in `params` using `std::make_tuple`. `std::forward` preserves the value category at submission. Ordinary lvalue arguments are copied into the tuple, and rvalues can be moved into it. A worker invokes this lambda later.
3. **Create shared state for the result.** A `std::packaged_task<ReturnType()>` wraps the zero-argument lambda. Its `get_future()` returns a future associated with the same state, where the task stores its result or exception.
4. **Make the task fit the queue.** `std::packaged_task` is move-only, and C++17 `std::function` requires a copyable callable. The queue stores a lambda that captures a `std::shared_ptr` to the packaged task by value, so the lambda can be copied.
5. **Notify a worker.** `submit()` checks `stopping` under the lock before enqueuing. If the flag is set, the check throws `std::runtime_error`. After a successful enqueue, it releases the lock, calls `cv.notify_one()`, and returns the future.

### How `std::apply` expands the arguments

If `params` contains `(3, 4)`, `std::apply` passes the two tuple elements as separate arguments to the inner lambda. That lambda calls the captured callable through `std::invoke`, which in this example is equivalent to calling `func(3, 4)`.

```cpp
return std::apply(
    [&func](auto&&... unpacked) -> ReturnType {
        return std::invoke(
            func,
            std::forward<decltype(unpacked)>(unpacked)...
        );
    },
    std::move(params)
);
```

`std::move(params)` passes the tuple as an rvalue, so ordinary value elements can reach the inner lambda as rvalues. Each named `unpacked` parameter is an lvalue expression when used directly, so `std::forward` preserves the element's incoming value category. The outer lambda is `mutable`, which allows its value captures to be modified or moved from during execution. This supports tasks that take a move-only argument such as `std::unique_ptr` by value.

## 4. How `work()` waits and runs tasks

A worker first acquires the mutex with `std::unique_lock<std::mutex>`. This lock supports both operations the condition variable needs: releasing the mutex while blocked and reacquiring it after wakeup. The two checks are shown below.

```cpp
cv.wait(lock, [this] {
    return stopping || !tasks.empty();
});

if (stopping && tasks.empty()) {
    return;
}
```

- **When waiting ends.** `wait()` can return when shutdown has started or the queue contains work. If the predicate is false, the wait releases the mutex while the worker is blocked so other threads can enqueue tasks or set the shutdown flag.
- **Why the worker checks again.** Condition variables can wake spuriously. Even after a notification, another worker may take the task before this worker reacquires the lock. The predicate form of `wait()` checks the condition again under the lock.
- **When the loop exits.** The worker exits only when shutdown has started and the queue is empty. Otherwise, it moves the front task into a local variable and pops it under the lock. It releases the lock before calling `task()`, allowing other threads to submit or take work while the task runs.

Tasks are dequeued in FIFO order. With multiple workers, the order in which tasks start, finish, or print can vary.

## 5. How the destructor shuts down the pool

1. The destructor acquires `mtx` and sets `stopping` to `true`.
2. It releases the lock and calls `cv.notify_all()` so waiting workers recheck the predicate.
3. Workers continue running queued tasks and exit once they see the shutdown flag and an empty queue.
4. The destructor joins each worker outside the lock. Even if the last task has been dequeued, `join()` waits for the worker executing it to finish.

Normal destruction therefore waits for accepted tasks to finish. If a task blocks indefinitely, destruction also waits indefinitely.

## 6. Exceptions and the repository example

If a task throws, `std::packaged_task` stores the exception in the shared state, and the worker can continue with later tasks. The exception is rethrown in the calling thread when it calls `future.get()`. An exception raised by `submit()` when its shutdown check fails goes directly to the submitting thread.

[`src/main.cpp`](src/main.cpp) creates a pool with one worker. It first submits a task that throws `std::runtime_error`, then submits 100 tasks that compute squares. The futures outlive the pool's scope. After the pool is destroyed, assertions check the exception and each result. Using one worker also verifies that the same worker can continue after a task throws. Keep assertions enabled when running this example as a test.

## 7. Usage limits of the current implementation

- **Use a positive worker count.** The constructor uses the count it receives, and `hardware_concurrency()` can return zero. With zero workers, jobs remain in the queue while the pool is alive.
- **Synchronize shared data inside tasks.** The pool's mutex protects the queue. Tasks that concurrently modify the same data or need to print complete log lines should synchronize that access themselves.
- **Finish submitting before destroying the pool.** Callers are responsible for keeping the pool and any data referenced by tasks alive. If a task uses `std::ref` or captures a reference, the referenced object must outlive the task's use of it.
- **Adjust the header before including it in multiple source files.** The example includes the header from one translation unit. For multiple `.cpp` files, mark the constructor, destructor, and `work()` definitions in the header `inline` or move them into a separate `.cpp` file.
