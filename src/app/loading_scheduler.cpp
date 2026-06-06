#include "app/loading_scheduler.h"

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace phoenix::app
{
    namespace
    {
        std::size_t hardware_threads()
        {
            return std::max(1u, std::thread::hardware_concurrency());
        }

        std::size_t cpu_worker_count()
        {
            const auto threads = hardware_threads();
            if (threads <= 4)
                return 1;
            if (threads <= 8)
                return 2;
            if (threads <= 12)
                return 3;
            return 4;
        }

        std::size_t io_worker_count()
        {
            return 1;
        }
    }

    void set_current_thread_loading_priority()
    {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
        nice(10);
#endif
    }

    LoadingScheduler::LoadingScheduler(LoadingWorkKind kind)
        : kind_(kind)
    {
        maxWorkers_ = kind_ == LoadingWorkKind::Cpu ? cpu_worker_count() : io_worker_count();
        minActiveWorkers_ = 1;
        activeWorkers_.store(maxWorkers_, std::memory_order_relaxed);

        workers_.reserve(maxWorkers_);
        for (std::size_t i = 0; i < maxWorkers_; ++i)
            workers_.emplace_back([this, i]() { worker_loop(i); });
    }

    LoadingScheduler::~LoadingScheduler()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable())
                worker.join();
    }

    void LoadingScheduler::note_loading_frame(std::chrono::milliseconds elapsed)
    {
        if (kind_ != LoadingWorkKind::Cpu || maxWorkers_ <= minActiveWorkers_)
            return;

        auto active = activeWorkers_.load(std::memory_order_relaxed);
        if (elapsed > std::chrono::milliseconds(55))
        {
            responsiveFrames_.store(0, std::memory_order_relaxed);
            if (active > minActiveWorkers_)
            {
                activeWorkers_.store(active - 1, std::memory_order_relaxed);
                cv_.notify_all();
            }
            return;
        }

        if (elapsed < std::chrono::milliseconds(24))
        {
            const auto stable = responsiveFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (stable >= 8 && active < maxWorkers_)
            {
                responsiveFrames_.store(0, std::memory_order_relaxed);
                activeWorkers_.store(active + 1, std::memory_order_relaxed);
                cv_.notify_all();
            }
        }
        else
        {
            responsiveFrames_.store(0, std::memory_order_relaxed);
        }
    }

    std::size_t LoadingScheduler::worker_count() const
    {
        return maxWorkers_;
    }

    std::size_t LoadingScheduler::active_worker_count() const
    {
        return activeWorkers_.load(std::memory_order_relaxed);
    }

    void LoadingScheduler::enqueue(std::function<void()> job)
    {
        {
            std::lock_guard lock(mutex_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

    void LoadingScheduler::worker_loop(std::size_t workerIndex)
    {
        set_current_thread_loading_priority();

        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&]() {
                    return stopping_
                        || (!jobs_.empty()
                            && workerIndex < activeWorkers_.load(std::memory_order_relaxed));
                });

                if (stopping_ && jobs_.empty())
                    return;
                if (workerIndex >= activeWorkers_.load(std::memory_order_relaxed))
                    continue;

                job = std::move(jobs_.front());
                jobs_.pop();
            }

            job();
            std::this_thread::sleep_for(kind_ == LoadingWorkKind::Io
                ? std::chrono::milliseconds(6)
                : std::chrono::milliseconds(2));
        }
    }
}
