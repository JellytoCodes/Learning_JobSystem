// =============================================================================
// Day 11 — Dependency Counter / Continuation
//
// 목표:
//   Day 10에서 확인한 "워커 안에서 Wait()하면 starvation이 생긴다"는 문제를
//   dependency counter + continuation 방식으로 해결한다.
//
// 핵심 아이디어:
//   후속 작업은 선행 작업을 Wait()하지 않는다.
//   선행 작업들이 끝날 때마다 atomic counter를 줄이고,
//   마지막 선행 작업이 끝나는 순간 후속 작업을 큐에 자동 제출한다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day11_DependencyCounter.cpp -I../src -o Day11
//   ./Day11
// =============================================================================

#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using Clock    = std::chrono::high_resolution_clock;
using Ms       = std::chrono::milliseconds;
using Duration = std::chrono::duration<double, std::milli>;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

static void PrintLog(const std::vector<std::string>& log)
{
    for (const auto& line : log)
        std::cout << "  " << line << "\n";
}

// =============================================================================
// 실험 1: A 완료 후 B 자동 실행
// =============================================================================
static void Experiment1_SingleDependency()
{
    std::cout << "\n[실험 1] A 완료 후 B 자동 실행\n";
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

    auto hA = pool.Submit([&Log]
    {
        SleepMs(80);
        Log("A 완료");
    });

    auto hB = pool.SubmitAfter({ hA }, [&Log]
    {
        Log("B 실행");
    });

    hB.Wait();
    Duration elapsed = Clock::now() - start;

    PrintLog(log);
    std::cout << "  hA.Wait() 없이 B가 자동 제출됨\n";
    std::cout << "  소요 시간: " << std::fixed << std::setprecision(1)
              << elapsed.count() << " ms\n";
}

// =============================================================================
// 실험 2: 여러 선행 작업이 모두 끝난 뒤 Fan-in 실행
// =============================================================================
static void Experiment2_FanIn()
{
    std::cout << "\n[실험 2] 여러 선행 작업 완료 후 Fan-in\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(4);
    constexpr int kParts = 6;

    std::vector<int> partialResults(kParts, 0);
    std::vector<JobHandle> dependencies;
    dependencies.reserve(kParts);

    auto start = Clock::now();

    for (int i = 0; i < kParts; ++i)
    {
        dependencies.push_back(pool.Submit([i, &partialResults]
        {
            SleepMs(30 + i * 10);
            partialResults[i] = (i + 1) * 10;
        }));
    }

    std::atomic<int> total{ 0 };
    auto fanIn = pool.SubmitAfter(dependencies, [&partialResults, &total]
    {
        total = std::accumulate(partialResults.begin(), partialResults.end(), 0);
    });

    fanIn.Wait();
    Duration elapsed = Clock::now() - start;

    const int expected = kParts * (kParts + 1) / 2 * 10;
    std::cout << "  선행 작업 수: " << kParts << "\n";
    std::cout << "  합산 결과: " << total.load() << " (예상: " << expected << ")\n";
    std::cout << "  Fan-in은 모든 partialResults가 채워진 뒤 한 번만 실행\n";
    std::cout << "  소요 시간: " << elapsed.count() << " ms\n";
}

// =============================================================================
// 실험 3: 포화된 워커에서도 Wait 없이 후속 작업 실행
//
// ThreadPool(2)에 Parent 2개를 넣는다.
// 각 Parent는 Child를 제출하고 즉시 반환한다.
// 모든 Child가 끝나면 Final 작업이 자동 제출된다.
//
// Day 10의 문제점:
//   Parent가 Child.Wait()로 워커를 붙잡으면 Child를 실행할 워커가 사라질 수 있다.
//
// Day 11 방식:
//   Parent는 기다리지 않고 반환하므로 워커 슬롯이 풀린다.
//   Final은 Child 핸들 2개가 모두 완료된 뒤 continuation으로 제출된다.
// =============================================================================
static void Experiment3_SaturatedWorkersWithoutWait()
{
    std::cout << "\n[실험 3] 포화된 워커에서도 Wait 없이 후속 작업 실행\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(2);
    std::vector<JobHandle> children;
    children.reserve(2);

    std::vector<std::string> log;
    std::mutex logMutex;
    auto Log = [&](const std::string& text)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(text);
    };

    std::atomic<int> childCount{ 0 };
    std::atomic<int> finalRan{ 0 };

    for (int parentIndex = 0; parentIndex < 2; ++parentIndex)
    {
        auto parent = pool.Submit([parentIndex, &Log]
        {
            SleepMs(10);
            Log("Parent " + std::to_string(parentIndex) + " 완료");
        });

        children.push_back(pool.SubmitAfter({ parent }, [parentIndex, &childCount, &Log]
        {
            SleepMs(40);
            ++childCount;
            Log("Child " + std::to_string(parentIndex) + " 완료");
        }));
    }

    auto final = pool.SubmitAfter(children, [&childCount, &finalRan, &Log]
    {
        finalRan = childCount.load();
        Log("Final 실행: Child 완료 수 = " + std::to_string(finalRan.load()));
    });

    final.Wait();

    PrintLog(log);
    std::cout << "  결과: FinalRan = " << finalRan.load() << " / 2\n";
    std::cout << "  핵심: 워커가 Wait로 막히지 않아 큐의 후속 작업이 계속 진행됨\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 11 — Dependency Counter / Continuation\n";
    std::cout << "=====================================================\n";

    Experiment1_SingleDependency();
    Experiment2_FanIn();
    Experiment3_SaturatedWorkersWithoutWait();

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  JobHandle::Wait       : 호출 스레드를 블록한다\n";
    std::cout << "  SubmitAfter           : 선행 작업 완료 후 후속 작업 자동 제출\n";
    std::cout << "  dependency counter    : 남은 선행 작업 수를 atomic으로 추적\n";
    std::cout << "  continuation          : 마지막 선행 작업 완료 시 실행할 후속 제출 로직\n";
    std::cout << "  엔진식 방향           : 워커를 막지 말고 작업 그래프를 큐로 흘려보낸다\n";
    std::cout << "=====================================================\n\n";

    return 0;
}
