#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WorkStealingPool
{
public:
    struct WorkerStats
    {
        uint32_t index = 0;
        uint64_t executed = 0;
        uint64_t stolen = 0;
        size_t queued = 0;
    };

    explicit WorkStealingPool(uint32_t workerCount)
        : _stop(false)
    {
        if (workerCount == 0)
        {
            workerCount = 1;
        }

        _workers.reserve(workerCount);
        for (uint32_t i = 0; i < workerCount; ++i)
        {
            _workers.push_back(std::make_unique<Worker>());
        }

        for (uint32_t i = 0; i < workerCount; ++i)
        {
            _workers[i]->thread = std::thread([this, i]()
            {
                WorkerLoop(i);
            });
        }
    }

    ~WorkStealingPool()
    {
        _stop.store(true, std::memory_order_release);
        _wakeCv.notify_all();

        for (std::unique_ptr<Worker>& worker : _workers)
        {
            if (worker->thread.joinable())
            {
                worker->thread.join();
            }
        }
    }

    uint32_t WorkerCount() const
    {
        return static_cast<uint32_t>(_workers.size());
    }

    void SubmitTo(uint32_t workerIdx, std::function<void(uint32_t)> job)
    {
        workerIdx %= WorkerCount();

        _pendingJobs.fetch_add(1, std::memory_order_acq_rel);

        {
            std::lock_guard<std::mutex> lock(_workers[workerIdx]->mutex);
            _workers[workerIdx]->queue.push_back(Job{std::move(job), workerIdx});
        }

        _wakeCv.notify_all();
    }

    void WaitAll()
    {
        std::unique_lock<std::mutex> lock(_waitMutex);
        _waitCv.wait(lock, [this]()
        {
            return _pendingJobs.load(std::memory_order_acquire) == 0;
        });
    }

    std::vector<WorkerStats> GetStats() const
    {
        std::vector<WorkerStats> stats;
        stats.reserve(_workers.size());

        for (uint32_t i = 0; i < _workers.size(); ++i)
        {
            const Worker& worker = *_workers[i];

            std::lock_guard<std::mutex> lock(worker.mutex);
            stats.push_back(WorkerStats{
                i,
                worker.executed.load(std::memory_order_relaxed),
                worker.stolen.load(std::memory_order_relaxed),
                worker.queue.size()
            });
        }

        return stats;
    }

private:
    struct Job
    {
        std::function<void(uint32_t)> func;
        uint32_t owner = 0;
    };

    struct Worker
    {
        mutable std::mutex mutex;
        std::deque<Job> queue;
        std::thread thread;
        std::atomic<uint64_t> executed{0};
        std::atomic<uint64_t> stolen{0};
    };

    bool TryPopOwn(uint32_t workerIdx, Job& outJob)
    {
        Worker& worker = *_workers[workerIdx];
        std::lock_guard<std::mutex> lock(worker.mutex);

        if (worker.queue.empty())
        {
            return false;
        }

        outJob = std::move(worker.queue.back());
        worker.queue.pop_back();
        return true;
    }

    bool TrySteal(uint32_t thiefIdx, Job& outJob)
    {
        const uint32_t count = WorkerCount();

        for (uint32_t offset = 1; offset < count; ++offset)
        {
            const uint32_t victimIdx = (thiefIdx + offset) % count;
            Worker& victim = *_workers[victimIdx];

            std::lock_guard<std::mutex> lock(victim.mutex);
            if (victim.queue.empty())
            {
                continue;
            }

            outJob = std::move(victim.queue.front());
            victim.queue.pop_front();
            return true;
        }

        return false;
    }

    void WorkerLoop(uint32_t workerIdx)
    {
        while (true)
        {
            Job job;
            if (TryPopOwn(workerIdx, job) || TrySteal(workerIdx, job))
            {
                job.func(workerIdx);

                Worker& worker = *_workers[workerIdx];
                worker.executed.fetch_add(1, std::memory_order_relaxed);
                if (job.owner != workerIdx)
                {
                    worker.stolen.fetch_add(1, std::memory_order_relaxed);
                }

                if (_pendingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    _waitCv.notify_all();
                }

                continue;
            }

            if (_stop.load(std::memory_order_acquire) &&
                _pendingJobs.load(std::memory_order_acquire) == 0)
            {
                return;
            }

            std::unique_lock<std::mutex> lock(_wakeMutex);
            _wakeCv.wait_for(lock, std::chrono::milliseconds(1));
        }
    }

private:
    std::vector<std::unique_ptr<Worker>> _workers;
    std::atomic<bool> _stop;
    std::atomic<uint64_t> _pendingJobs{0};

    std::mutex _waitMutex;
    std::condition_variable _waitCv;

    std::mutex _wakeMutex;
    std::condition_variable _wakeCv;
};

namespace
{
    void PrintStats(const std::string& title, const WorkStealingPool& pool)
    {
        std::cout << "\n[" << title << "]\n";
        std::cout << "worker | executed | stolen | queued\n";
        std::cout << "-------+----------+--------+-------\n";

        for (const WorkStealingPool::WorkerStats& stat : pool.GetStats())
        {
            std::cout << std::setw(6) << stat.index << " | "
                      << std::setw(8) << stat.executed << " | "
                      << std::setw(6) << stat.stolen << " | "
                      << std::setw(5) << stat.queued << "\n";
        }
    }

    void Experiment1_EvenDistribution()
    {
        std::cout << "=== Experiment 1: even distribution ===\n";
        WorkStealingPool pool(4);

        for (uint32_t worker = 0; worker < pool.WorkerCount(); ++worker)
        {
            for (uint32_t i = 0; i < 8; ++i)
            {
                pool.SubmitTo(worker, [](uint32_t)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                });
            }
        }

        pool.WaitAll();
        PrintStats("균등 분배에서는 steal 필요가 작다", pool);
    }

    void Experiment2_OverloadedWorker()
    {
        std::cout << "\n=== Experiment 2: overloaded worker ===\n";
        WorkStealingPool pool(4);

        for (uint32_t i = 0; i < 32; ++i)
        {
            pool.SubmitTo(0, [](uint32_t)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            });
        }

        pool.WaitAll();
        PrintStats("worker 0 큐를 다른 worker들이 훔쳐 처리한다", pool);
    }

    void Experiment3_LifoAndFifo()
    {
        std::cout << "\n=== Experiment 3: owner LIFO, thief FIFO ===\n";
        WorkStealingPool pool(4);

        std::mutex logMutex;
        std::vector<std::string> log;
        log.reserve(10);

        for (uint32_t jobId = 0; jobId < 10; ++jobId)
        {
            pool.SubmitTo(0, [jobId, &logMutex, &log](uint32_t executedBy)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));

                std::lock_guard<std::mutex> lock(logMutex);
                log.push_back("job " + std::to_string(jobId) +
                    " -> worker " + std::to_string(executedBy));
            });
        }

        pool.WaitAll();
        PrintStats("owner는 뒤에서 pop, thief는 앞에서 steal", pool);

        std::cout << "\nexecution log:\n";
        for (const std::string& line : log)
        {
            std::cout << "  " << line << "\n";
        }
    }
}

int main()
{
    std::cout << "Day 16 - Work Stealing\n";
    std::cout << "핵심: 각 worker는 local queue를 우선 처리하고, 비면 다른 worker queue 앞쪽에서 steal한다.\n";

    Experiment1_EvenDistribution();
    Experiment2_OverloadedWorker();
    Experiment3_LifoAndFifo();

    std::cout << "\n정리: 전역 큐 하나에 몰리는 구조보다 불균형 완화와 worker 활용률 측면에서 유리하다.\n";
    return 0;
}
