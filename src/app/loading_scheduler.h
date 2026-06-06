#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace phoenix::app
{
    enum class LoadingWorkKind
    {
        Cpu,
        Io,
    };

    void set_current_thread_loading_priority();

    class LoadingScheduler
    {
    public:
        explicit LoadingScheduler(LoadingWorkKind kind);
        ~LoadingScheduler();

        LoadingScheduler(const LoadingScheduler&) = delete;
        LoadingScheduler& operator=(const LoadingScheduler&) = delete;

        template <typename Fn>
        auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>>
        {
            using Result = std::invoke_result_t<Fn>;
            auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
            auto future = task->get_future();
            enqueue([task]() { (*task)(); });
            return future;
        }

        void note_loading_frame(std::chrono::milliseconds elapsed);
        std::size_t worker_count() const;
        std::size_t active_worker_count() const;

    private:
        void enqueue(std::function<void()> job);
        void worker_loop(std::size_t workerIndex);

        LoadingWorkKind kind_{};
        std::size_t minActiveWorkers_{ 1 };
        std::size_t maxWorkers_{ 1 };
        std::atomic<std::size_t> activeWorkers_{ 1 };
        std::atomic<std::uint32_t> responsiveFrames_{ 0 };
        std::vector<std::thread> workers_;
        std::queue<std::function<void()>> jobs_;
        mutable std::mutex mutex_;
        std::condition_variable cv_;
        bool stopping_{ false };
    };

    template <typename Fn>
    void parallel_for_loading(LoadingScheduler& scheduler, std::size_t count, Fn&& fn)
    {
        if (count == 0)
            return;

        const auto workerCount = std::min(count, scheduler.worker_count());
        if (workerCount <= 1)
        {
            for (std::size_t i = 0; i < count; ++i)
                fn(i);
            return;
        }

        std::atomic<std::size_t> nextIndex{ 0 };
        std::vector<std::future<void>> futures;
        futures.reserve(workerCount);
        for (std::size_t worker = 0; worker < workerCount; ++worker)
        {
            futures.push_back(scheduler.submit([&nextIndex, count, &fn]() {
                for (;;)
                {
                    const auto index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (index >= count)
                        break;
                    fn(index);
                }
            }));
        }

        for (auto& future : futures)
            future.get();
    }
}
