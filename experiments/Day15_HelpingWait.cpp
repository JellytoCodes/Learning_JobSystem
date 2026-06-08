// =============================================================================
// Day 15 — Helping Wait
//
// 목표:
//   Day 10에서 확인한 worker wait starvation을 완화하는 방법으로
//   "기다리는 동안 큐의 다른 작업을 직접 실행하는" helping wait를 실험한다.
//
// 핵심 아이디어:
//   JobHandle::Wait()는 호출 스레드를 잠재우지만,
//   ThreadPool::WaitWithHelping(handle)은 handle이 끝날 때까지
//   큐에 남은 작업을 하나씩 꺼내 현재 스레드에서 실행한다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day15_HelpingWait.cpp -I../src -o Day15
//   ./Day15
// =============================================================================

#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using Ms    = std::chrono::milliseconds;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

static bool WaitForForDemo(const JobHandle& handle, Ms timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (!handle.IsDone())
    {
        if (Clock::now() >= deadline)
            return false;
        SleepMs(1);
    }
    return true;
}

static void PrintLog(const std::vector<std::string>& log)
{
    for (const auto& line : log)
        std::cout << "  " << line << "\n";
}

// =============================================================================
// 실험 1: 일반 Wait는 워커를 점유한다
// =============================================================================
static void Experiment1_NormalWaitCanStarve()
{
    std::cout << "\n[실험 1] 일반 Wait — 포화된 워커에서 starvation 위험\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::atomic<int> childrenCompleted{ 0 };
    std::vector<std::string> log;
    std::mutex logMutex;

    auto Log = [&](const std::string& text)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(text);
    };

    for (int parentIndex = 0; parentIndex < 2; ++parentIndex)
    {
        (void)pool.Submit([parentIndex, &pool, &childrenCompleted, &Log]
        {
            Log("Parent " + std::to_string(parentIndex) + ": Child 제출");

            auto child = pool.Submit([parentIndex, &childrenCompleted, &Log]
            {
                SleepMs(30);
                ++childrenCompleted;
                Log("Child " + std::to_string(parentIndex) + ": 완료");
            });

            const bool done = WaitForForDemo(child, Ms(150));
            if (!done)
            {
                Log("Parent " + std::to_string(parentIndex)
                    + ": 150ms 동안 Child 미완료 (진짜 Wait()면 여기서 정지 가능)");
            }
        });
    }

    pool.WaitAll();

    PrintLog(log);
    std::cout << "  Child 완료 수: " << childrenCompleted.load() << " / 2\n";
    std::cout << "  해석: 워커가 기다리기만 하면 큐의 Child를 꺼낼 실행자가 부족해진다\n";
}

// =============================================================================
// 실험 2: WaitWithHelping은 기다리는 동안 큐 작업을 직접 실행한다
// =============================================================================
static void Experiment2_HelpingWaitMakesProgress()
{
    std::cout << "\n[실험 2] WaitWithHelping — 대기 중 작업 돕기\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::atomic<int> childrenCompleted{ 0 };
    std::atomic<int> parentsCompleted{ 0 };
    std::vector<std::string> log;
    std::mutex logMutex;

    auto Log = [&](const std::string& text)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(text);
    };

    for (int parentIndex = 0; parentIndex < 2; ++parentIndex)
    {
        (void)pool.Submit([parentIndex, &pool, &childrenCompleted, &parentsCompleted, &Log]
        {
            Log("Parent " + std::to_string(parentIndex) + ": Child 제출");

            auto child = pool.Submit([parentIndex, &childrenCompleted, &Log]
            {
                SleepMs(30);
                ++childrenCompleted;
                Log("Child " + std::to_string(parentIndex) + ": 완료");
            });

            pool.WaitWithHelping(child);
            ++parentsCompleted;
            Log("Parent " + std::to_string(parentIndex) + ": WaitWithHelping 반환");
        });
    }

    pool.WaitAll();

    PrintLog(log);
    std::cout << "  Parent 완료 수: " << parentsCompleted.load() << " / 2\n";
    std::cout << "  Child 완료 수 : " << childrenCompleted.load() << " / 2\n";
    std::cout << "  해석: 기다리는 워커도 큐 작업을 처리하므로 starvation 위험이 줄어든다\n";
}

// =============================================================================
// 실험 3: Helping Wait도 예외는 일반 Wait와 동일하게 재전파한다
// =============================================================================
static void Experiment3_HelpingWaitRethrows()
{
    std::cout << "\n[실험 3] WaitWithHelping 예외 재전파\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::atomic<int> caught{ 0 };

    auto parent = pool.Submit([&pool, &caught]
    {
        auto child = pool.Submit([]
        {
            throw std::runtime_error("Child 실패");
        });

        try
        {
            pool.WaitWithHelping(child);
        }
        catch (const std::exception& e)
        {
            ++caught;
            std::cout << "  Parent가 Child 예외 확인: " << e.what() << "\n";
        }
    });

    parent.Wait();

    std::cout << "  caught = " << caught.load() << " / 1\n";
    std::cout << "  해석: helping wait는 스케줄링 정책일 뿐, 완료/예외 의미는 JobHandle::Wait와 같다\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 15 — Helping Wait\n";
    std::cout << "=====================================================\n";

    Experiment1_NormalWaitCanStarve();
    Experiment2_HelpingWaitMakesProgress();
    Experiment3_HelpingWaitRethrows();

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  JobHandle::Wait      : 호출 스레드를 블록\n";
    std::cout << "  WaitWithHelping      : 대기 중 큐 작업을 직접 실행\n";
    std::cout << "  starvation 완화      : 기다리는 워커도 진행에 기여\n";
    std::cout << "  다음 단계            : worker local queue / work stealing\n";
    std::cout << "=====================================================\n\n";

    return 0;
}
