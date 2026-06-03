// =============================================================================
// Day 10 — 워커 안에서 Wait() 하기: Starvation / Deadlock
//
// 목표:
//   JobHandle로 단순 의존성을 표현할 수는 있지만,
//   워커 스레드 안에서 다른 작업을 Wait()하면 풀 전체가 멈출 수 있음을 확인한다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day10_WorkerWaitStarvation.cpp -I../src -o Day10
//   ./Day10
// =============================================================================

#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock    = std::chrono::high_resolution_clock;
using Ms       = std::chrono::milliseconds;
using Duration = std::chrono::duration<double, std::milli>;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

// =============================================================================
// 데모 전용 WaitFor
//
// JobHandle::Wait()는 타임아웃이 없으므로, 데드락 실험에서 프로그램이 멈추지 않게
// IsDone()을 짧게 폴링한다. 실제 엔진 코드라면 condition_variable 기반 timed wait나
// work stealing/helping wait를 별도 설계해야 한다.
// =============================================================================
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
// 실험 1: 메인 스레드에서 Wait() — 안전한 대기
// =============================================================================
static void Experiment1_MainThreadWait()
{
    std::cout << "\n[실험 1] 메인 스레드에서 Wait()\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    auto start = Clock::now();

    auto handle = pool.Submit([] { SleepMs(80); });
    handle.Wait();

    Duration elapsed = Clock::now() - start;
    std::cout << "  메인 스레드가 기다리는 동안 워커는 계속 작업 가능\n";
    std::cout << "  대기 시간: " << std::fixed << std::setprecision(1)
              << elapsed.count() << " ms\n";
}

// =============================================================================
// 실험 2: 워커가 자식 작업을 기다리지만 여유 워커가 있는 경우
// =============================================================================
static void Experiment2_WorkerWaitWithSpareThread()
{
    std::cout << "\n[실험 2] 워커 Wait + 여유 워커 1개\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::vector<std::string> log;
    std::mutex logMutex;

    auto Log = [&](const std::string& text)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(text);
    };

    auto start = Clock::now();

    auto parent = pool.Submit([&pool, &Log]
    {
        Log("Parent 시작: Child 제출");
        auto child = pool.Submit([&Log]
        {
            SleepMs(80);
            Log("Child 완료");
        });

        Log("Parent: Child.Wait() 진입");
        child.Wait();
        Log("Parent 완료");
    });

    parent.Wait();
    Duration elapsed = Clock::now() - start;

    PrintLog(log);
    std::cout << "  결과: 2개 워커 중 하나가 Child를 실행해서 완료 가능\n";
    std::cout << "  소요 시간: " << elapsed.count() << " ms\n";
}

// =============================================================================
// 실험 3: 모든 워커가 Wait()에 들어가면 큐의 자식 작업이 실행되지 못한다
//
// ThreadPool(2)에 Parent 2개를 넣고, 각 Parent가 Child를 제출한 뒤 기다린다.
// 두 워커가 모두 Parent 안에서 대기하면 Child를 꺼낼 워커가 남지 않는다.
//
// 실제 child.Wait()를 쓰면 데모가 멈추므로 WaitForForDemo로 150ms 후 빠져나온다.
// Parent가 포기하고 반환한 뒤에야 워커가 풀려 Child가 실행된다.
// =============================================================================
static void Experiment3_SaturatedWorkerWait()
{
    std::cout << "\n[실험 3] 모든 워커가 Wait()에 막히는 상황\n";
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
                    + ": 150ms 동안 Child 미완료 (진짜 Wait()면 여기서 정지)");
            }
        });
    }

    pool.WaitAll();

    PrintLog(log);
    std::cout << "  Child 완료 수: " << childrenCompleted.load() << " / 2\n";
    std::cout << "  핵심: 워커를 대기 상태로 점유하면 큐에 일이 있어도 실행자가 사라진다\n";
}

// =============================================================================
// 실험 4: 대안 — 워커는 기다리지 말고 다음 작업을 제출한 뒤 반환
// =============================================================================
static void Experiment4_SubmitAndReturn()
{
    std::cout << "\n[실험 4] 대안: Submit 후 반환\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::atomic<int> childrenCompleted{ 0 };

    auto start = Clock::now();

    for (int parentIndex = 0; parentIndex < 2; ++parentIndex)
    {
        (void)pool.Submit([parentIndex, &pool, &childrenCompleted]
        {
            // Parent는 Child를 제출하고 즉시 반환한다.
            // 워커를 붙잡지 않으므로 다른 큐 작업을 계속 처리할 수 있다.
            (void)pool.Submit([parentIndex, &childrenCompleted]
            {
                SleepMs(30 + parentIndex * 10);
                ++childrenCompleted;
            });
        });
    }

    pool.WaitAll();
    Duration elapsed = Clock::now() - start;

    std::cout << "  Child 완료 수: " << childrenCompleted.load() << " / 2\n";
    std::cout << "  소요 시간: " << elapsed.count() << " ms\n";
    std::cout << "  다음 단계: 의존성 카운터로 '모든 선행 작업 완료 시 자동 제출' 만들기\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 10 — Worker Wait Starvation\n";
    std::cout << "=====================================================\n";

    Experiment1_MainThreadWait();
    Experiment2_WorkerWaitWithSpareThread();
    Experiment3_SaturatedWorkerWait();
    Experiment4_SubmitAndReturn();

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  메인 스레드 Wait       : 보통 안전한 완료 대기\n";
    std::cout << "  워커 스레드 Wait       : 워커 슬롯을 점유하므로 starvation 위험\n";
    std::cout << "  모든 워커가 Wait       : 큐의 후속 작업을 실행할 스레드가 사라짐\n";
    std::cout << "  다음 단계              : dependency counter / continuation\n";
    std::cout << "=====================================================\n\n";

    return 0;
}
