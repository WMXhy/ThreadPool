# C++17 线程池

[English](README.en.md)

一个固定大小的线程池。构造时启动一组工作线程，调用方用 `submit()` 提交任务并取得 `std::future`。工作线程从共享队列取任务，在锁外执行。线程池析构时会处理完已入队的任务，再等待所有工作线程退出。

- [`include/ThreadPool.hpp`](include/ThreadPool.hpp) 包含线程池的完整实现。
- [`src/main.cpp`](src/main.cpp) 演示如何验证任务结果、异常传递和析构行为。
- [`ThreadPool.drawio.png`](ThreadPool.drawio.png) 是线程池的设计图。

## 1. 提交任务

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

`submit(f, args...)` 可以接收带参数的可调用对象。普通的带返回值任务会得到 `std::future<T>`，返回 `void` 的任务会得到 `std::future<void>`。调用 `get()` 时，如果结果还没就绪，调用线程会等待。每个 `std::future` 的结果只能用 `get()` 取一次。

## 2. 成员变量和工作线程

| 成员 | 职责 |
| --- | --- |
| `workers` | 保存构造时启动的 `std::thread`，供析构时逐个 `join()`。 |
| `tasks` | 以 FIFO 顺序保存 `std::function<void()>`。不同签名的任务会包装成统一的无参调用。 |
| `mtx` | 保护队列的检查、入队和出队。修改关闭状态和检查等待谓词也使用这把锁。 |
| `cv` | 让空闲线程等待，提交任务或关闭时再通知它们。 |
| `stopping` | 初始为 `false`，析构时设为 `true`。它是原子变量，和队列相关的判断仍在持锁时完成。 |
| `threadSize` | 记录固定的工作线程数。构造时可显式传入，默认取 `std::thread::hardware_concurrency()`。 |

构造函数通过 `workers.emplace_back([this] { work(); })` 启动工作线程。每个线程都运行同一个 `work()` 循环，完成一个任务后回到队列等待或领取下一项任务。

## 3. `submit()` 如何把任务放进队列

1. **推导结果类型。** 实现用 `std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>` 计算 `ReturnType`，并在接口中返回 `std::future<ReturnType>`。
2. **保存函数和参数。** 外层无参 lambda 把可调用对象存进 `func`，用 `std::make_tuple` 把参数存进 `params`。`std::forward` 保留提交时的值类别，普通左值参数会被复制进元组，右值可以被移动进去。工作线程稍后调用这个 lambda。
3. **为任务建立共享状态。** `std::packaged_task<ReturnType()>` 包装无参 lambda，`get_future()` 返回对应的 future。任务执行的结果或异常会写入两者共享的状态。
4. **把任务放进队列。** `std::packaged_task` 只能移动，而 C++17 的 `std::function` 要求它保存的可调用对象可复制。这里用 `std::shared_ptr` 持有 packaged task，入队的 lambda 通过值捕获这个指针，因此可以复制。
5. **通知工作线程。** `submit()` 在锁内检查 `stopping` 并入队。如果已处于关闭状态，这里的检查会抛出 `std::runtime_error`。成功入队后先释放锁，再调用 `cv.notify_one()`，最后返回 future。

### `std::apply` 怎样展开参数

例如 `params` 保存了 `(3, 4)`，`std::apply` 会把这两个元素分别交给内部 lambda。内部 lambda 用 `std::invoke` 调用捕获的函数，效果相当于 `func(3, 4)`。

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

`std::move(params)` 把元组作为右值传给 `apply`，使普通的按值存储元素可以作为右值传给内部 lambda。`unpacked` 是有名字的参数，直接使用时是左值表达式，所以要用 `std::forward` 保留元素传进来时的值类别。外层 lambda 的 `mutable` 让这些按值捕获的对象在执行时可以被修改或移动。这使得任务可以按值接收 `std::unique_ptr` 之类只能移动的参数。

## 4. `work()` 怎样等待和执行任务

工作线程先用 `std::unique_lock<std::mutex>` 拿到锁。条件变量需要在阻塞时释放锁、唤醒后重新拿锁，所以这里用支持这两种操作的 `unique_lock`。核心判断如下。

```cpp
cv.wait(lock, [this] {
    return stopping || !tasks.empty();
});

if (stopping && tasks.empty()) {
    return;
}
```

- **何时结束等待。** 停止标志为真，或者队列里已有任务，`wait()` 就可以返回。谓词为假时，`wait()` 会释放互斥锁并阻塞，让其他线程能入队或设置关闭状态。
- **为什么醒来后还要检查。** 条件变量可能虚假唤醒。即使确实收到了通知，线程重新拿到锁前，任务也可能已经被其他工作线程取走。带谓词的 `wait()` 会在持锁时重新检查条件。
- **何时退出循环。** 只有 `stopping` 为真且队列为空，工作线程才退出。否则，它在锁内把队首任务移到局部变量，再将任务从队列移除。离开作用域释放锁后才调用 `task()`，让其他线程能同时提交或领取任务。

队列保证取任务时的 FIFO 顺序。多个工作线程同时运行时，任务的实际开始、完成和输出顺序都可能不同。

## 5. 析构怎样关闭线程池

1. 析构函数持有 `mtx`，将 `stopping` 设为 `true`。
2. 释放锁后调用 `cv.notify_all()`，让等待中的工作线程重新检查谓词。
3. 队列里的任务继续执行。工作线程在看到关闭标志且队列已空时退出。
4. 析构函数在锁外逐个 `join()`。即使最后一项任务已出队，`join()` 也会等待正在执行它的线程结束。

所以，正常析构会等已接受的任务执行完。任务如果一直阻塞，析构也会一直等下去。

## 6. 异常传递与仓库示例

任务函数抛出异常时，`std::packaged_task` 把异常保存到共享状态，工作线程可以继续处理后续任务。调用方执行 `future.get()` 时，异常会在调用方线程里重新抛出。相比之下，`submit()` 在入队阶段检测到关闭状态时，会直接向提交者抛出异常。

[`src/main.cpp`](src/main.cpp) 创建一个只有一个工作线程的池。它先提交一个会抛出 `std::runtime_error` 的任务，再提交 100 个求平方的任务。示例把 future 保存在池的作用域外，等池析构后，通过断言检查异常和每个平方结果。单线程设置也验证了同一个工作线程在遇到任务异常后还能继续工作。这个示例使用 `assert`，验证时应保留断言。

## 7. 当前实现的使用边界

- **线程数应大于 0。** 目前构造函数会照用传入的线程数，而 `hardware_concurrency()` 也可能返回 0。没有工作线程时，任务在池存活期间会一直留在队列里。
- **任务自身的共享数据需要同步。** 池的互斥锁保护队列。任务并发修改同一份数据，或者想让每条日志完整打印一行时，应在任务侧使用对应的同步措施。
- **先结束提交，再销毁池。** 调用方负责保证线程池对象和任务引用的数据仍然有效。若用 `std::ref` 传参或在任务中捕获引用，被引用对象必须活到任务执行完。
- **多源文件使用需要调整头文件组织。** 当前示例只在一个翻译单元中包含头文件。如果多个 `.cpp` 同时包含它，头文件中的构造函数、析构函数和 `work()` 的定义需要标为 `inline`，或移到单独的 `.cpp` 中。
