#include <thread>
#include <queue>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <future>
#include <functional>

class ThreadPool{
    private:
    std::vector<std::thread> workers;                   //工作线程
    std::queue<std::function<void()>> tasks;            //任务队列

    std::mutex mtx;
    std::condition_variable cv;

    std::atomic<bool> stopping = false;
    const size_t threadSize;

    public:

    ThreadPool(size_t _threadSize = std::thread::hardware_concurrency());
    ~ThreadPool();

    void work();

    template<class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<
            std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>
        >;
};

ThreadPool::ThreadPool(size_t _threadSize) : threadSize(_threadSize){
    for (size_t i = 0; i < threadSize; i++){
        workers.emplace_back([this](){
            work();
        });
    }
}

ThreadPool::~ThreadPool(){
    {
        std::lock_guard<std::mutex> lock(mtx);
        stopping = true;
    }

    cv.notify_all();

    for (auto& worker : workers){
        if (worker.joinable()){
            worker.join();
        }
    }
}

void ThreadPool::work(){
    while (true){
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx);

            //等待检查是否结束
            cv.wait(lock, [this](){
                return stopping || !tasks.empty();
            });

            if (stopping && tasks.empty()) return;

            //提取任务
            task = std::move(tasks.front());
            tasks.pop();
        }
        task();
    }
}

template<class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<
        std::invoke_result_t<
            std::decay_t<F>,
            std::decay_t<Args>...
        >
    >
{
    using ReturnType = std::invoke_result_t<
        std::decay_t<F>,
        std::decay_t<Args>...
    >;

    // 1. 将函数和参数绑定成一个无参任务
    auto boundTask = [
        func = std::forward<F>(f),
        params = std::make_tuple(
            std::forward<Args>(args)...
        )
    ]() mutable -> ReturnType {

        return std::apply(
            [&func](auto&&... unpacked) -> ReturnType {
                return std::invoke(
                    func,
                    std::forward<decltype(unpacked)>(unpacked)...
                );
            },
            std::move(params)
        );
    };

    // 2. 使用 packaged_task 包装
    auto task = std::make_shared<
        std::packaged_task<ReturnType()>
    >(std::move(boundTask));

    // 3. 获取 future
    auto future = task->get_future();

    // 4. 放入任务队列
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (stopping) {
            throw std::runtime_error("ThreadPool is stopping");
        }

        tasks.emplace([task]() {
            (*task)();
        });
    }

    // 5. 唤醒工作线程
    cv.notify_one();

    return future;
}