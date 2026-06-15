// =============================================================================
// Day 22 — ThreadPool Local Queue Integration
//
// 목표:
//   Day16에서 독립 실험했던 worker local queue + steal 구조를 실제 ThreadPool의
//   제출/실행 경로에 작게 통합한다.
//
// 관찰 포인트:
//   - 외부 스레드 Submit은 global queue로 들어간다.
//   - worker 내부 Submit은 해당 worker local queue로 들어간다.
//   - 쉬는 worker는 다른 worker local queue 앞쪽에서 steal한다.
// =============================================================================

#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Ms = std::chrono::milliseconds;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

static void PrintQueueStats(const std::string& title, const ThreadPool& pool)
{
    std::cout << "\n[" << title << "]\n";
    std::cout << "worker | processed | global | local | steal\n";
    std::cout << "-------+-----------+--------+-------+------\n";

    for (const ThreadPool::QueueStats& stat : pool.GetQueueStats())
    {
        std::cout << std::setw(6) << stat.threadIdx << " | "
                  << std::setw(9) << stat.jobsProcessed << " | "
                  << std::setw(6) << stat.globalPops << " | "
                  << std::setw(5) << stat.localPops << " | "
                  << std::setw(5) << stat.steals << "\n";
    }
}

static uint64_t SumGlobalPops(const ThreadPool& pool)
{
    uint64_t total = 0;
    for (const ThreadPool::QueueStats& stat : pool.GetQueueStats())
        total += stat.globalPops;
    return total;
}

static uint64_t SumLocalPops(const ThreadPool& pool)
{
    uint64_t total = 0;
    for (const ThreadPool::QueueStats& stat : pool.GetQueueStats())
        total += stat.localPops;
    return total;
}

static uint64_t SumSteals(const ThreadPool& pool)
{
    uint64_t total = 0;
    for (const ThreadPool::QueueStats& stat : pool.GetQueueStats())
        total += stat.steals;
    return total;
}

static void Experiment1_ExternalSubmitUsesGlobalQueue()
{
    std::cout << "\n[실험 1] 외부 Submit은 global queue 사용\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(4);
    std::atomic<int> completed{0};

    for (int i = 0; i < 24; ++i)
    {
        (void)pool.Submit([&completed]
        {
            SleepMs(2);
            ++completed;
        });
    }

    pool.WaitAll();
    PrintQueueStats("external submit", pool);

    std::cout << "  completed: " << completed.load() << "\n";
    std::cout << "  global pops: " << SumGlobalPops(pool) << "\n";
    std::cout << "  local pops: " << SumLocalPops(pool) << "\n";
}

static void Experiment2_WorkerSubmitUsesLocalQueue()
{
    std::cout << "\n[실험 2] worker 내부 Submit은 local queue 사용\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(4);
    std::atomic<int> childCompleted{0};

    (void)pool.Submit([&pool, &childCompleted]
    {
        for (int i = 0; i < 48; ++i)
        {
            (void)pool.Submit([&childCompleted]
            {
                SleepMs(3);
                ++childCompleted;
            });
        }
    });

    pool.WaitAll();
    PrintQueueStats("nested submit", pool);

    std::cout << "  child completed: " << childCompleted.load() << "\n";
    std::cout << "  local pops: " << SumLocalPops(pool) << "\n";
    std::cout << "  steals: " << SumSteals(pool) << "\n";
}

static void Experiment3_OverloadedLocalQueueGetsStolen()
{
    std::cout << "\n[실험 3] 과부하 local queue를 다른 worker가 steal\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(4);
    std::atomic<int> childCompleted{0};

    (void)pool.Submit([&pool, &childCompleted]
    {
        for (int i = 0; i < 96; ++i)
        {
            (void)pool.Submit([&childCompleted]
            {
                SleepMs(4);
                ++childCompleted;
            });
        }

        // parent가 조금 더 점유되어 있으면 다른 worker가 local queue를 steal할 여지가 커진다.
        SleepMs(60);
    });

    pool.WaitAll();
    PrintQueueStats("overloaded local queue", pool);

    std::cout << "  child completed: " << childCompleted.load() << "\n";
    std::cout << "  steals: " << SumSteals(pool) << "\n";
    std::cout << "  핵심: owner가 처리하지 못한 local 작업을 idle worker가 가져간다.\n";
}

int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 22 — ThreadPool Local Queue Integration\n";
    std::cout << "=====================================================\n";

    Experiment1_ExternalSubmitUsesGlobalQueue();
    Experiment2_WorkerSubmitUsesLocalQueue();
    Experiment3_OverloadedLocalQueueGetsStolen();

    std::cout << "\n오늘의 핵심\n";
    std::cout << "  - 외부 Submit은 global queue로 들어간다.\n";
    std::cout << "  - worker 내부 Submit은 worker local queue로 들어간다.\n";
    std::cout << "  - local queue는 owner LIFO, thief FIFO 방향으로 처리한다.\n";
    return 0;
}
