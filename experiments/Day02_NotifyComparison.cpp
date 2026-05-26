// =============================================================================
// Day 02 — notify_one vs notify_all 성능 비교
//
// 목표:
//   Submit 시 notify_one과 notify_all의 실제 동작 차이와
//   성능 영향을 수치로 확인한다.
//
// 핵심 개념: Thundering Herd(우르르 몰려드는 떼) 문제
//   notify_all로 N개 스레드를 모두 깨우면,
//   N개가 동시에 mutex를 얻으려고 경쟁한다.
//   결국 1개만 작업을 가져가고 나머지 N-1개는 다시 잠든다.
//   이 과정에서 컨텍스트 스위칭 + mutex 경합 비용이 발생한다.
//
// 실행 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread Day02_NotifyComparison.cpp -o Day02 && ./Day02
// =============================================================================

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <vector>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <string>
#include <numeric>

using Clock    = std::chrono::high_resolution_clock;
using Ms       = std::chrono::milliseconds;
using Duration = std::chrono::duration<double, std::milli>;

// =============================================================================
// 측정 지표
// =============================================================================
struct Stats
{
    std::atomic<uint64_t> totalWakeups{0};    // 깨어난 총 횟수
    std::atomic<uint64_t> wastedWakeups{0};   // 깨어났지만 큐가 비어 다시 잠든 횟수
    std::atomic<uint64_t> processedJobs{0};   // 실제 처리한 작업 수
};

// =============================================================================
// 미니 워커 풀 (notify 방식만 다르게 실험)
// =============================================================================
struct WorkerPool
{
    std::queue<std::function<void()>> queue;
    std::mutex                        mtx;
    std::condition_variable           cv;
    std::atomic<bool>                 stop{ false };
    std::vector<std::thread>          workers;
    Stats                             stats;
    bool                              useNotifyAll;  // true: notify_all / false: notify_one

    explicit WorkerPool(uint32_t threadCount, bool notifyAll)
        : useNotifyAll(notifyAll)
    {
        workers.reserve(threadCount);
        for (uint32_t i = 0; i < threadCount; ++i)
            workers.emplace_back([this] { Loop(); });
    }

    ~WorkerPool()
    {
        {
            std::unique_lock<std::mutex> lock(mtx);
            stop = true;
        }
        // 종료 시에는 항상 notify_all — 모든 스레드가 종료 신호를 봐야 하므로
        cv.notify_all();
        for (auto& w : workers) w.join();
    }

    void Submit(std::function<void()> job)
    {
        {
            std::unique_lock<std::mutex> lock(mtx);
            queue.push(std::move(job));
        }

        if (useNotifyAll)
        {
            // ❌ notify_all: 스레드가 N개면 N개 전부 깨어난다.
            //    작업은 1개뿐이므로 1개만 가져가고 N-1개는 다시 잠든다.
            //    이 N-1번의 깨어남 + 락 경쟁 + 다시 잠드는 과정이 순수 낭비.
            cv.notify_all();
        }
        else
        {
            // ✅ notify_one: 잠든 스레드 1개만 깨운다.
            //    작업이 1개이므로 딱 1개만 깨우면 충분하다.
            cv.notify_one();
        }
    }

    void WaitAll()
    {
        // 모든 작업 완료 대기 — 간단히 폴링으로 구현 (Day 01 ThreadPool의 WaitAll과 동일 원리)
        while (stats.processedJobs.load() < /* submitted count — 아래서 주입 */ 0) {}
    }

private:
    void Loop()
    {
        while (true)
        {
            std::function<void()> job;

            {
                std::unique_lock<std::mutex> lock(mtx);

                ++stats.totalWakeups;  // wait에서 깨어난 순간

                cv.wait(lock, [this] { return !queue.empty() || stop.load(); });

                if (stop && queue.empty()) return;

                // 깨어났는데 큐가 비어 있으면 → 낭비된 wakeup
                // (predicate가 있으므로 실제로 비어있는 채로 통과하진 않지만,
                //  notify_all 시 경쟁에서 진 스레드들이 이 시점에 큐가 이미 비어있을 수 있다)
                if (queue.empty())
                {
                    ++stats.wastedWakeups;
                    continue;
                }

                job = std::move(queue.front());
                queue.pop();
            }

            job();
            ++stats.processedJobs;
        }
    }
};

// =============================================================================
// 실험 실행
// =============================================================================
static void RunExperiment(const std::string& label,
                           bool               useNotifyAll,
                           uint32_t           threadCount,
                           uint32_t           jobCount)
{
    WorkerPool pool(threadCount, useNotifyAll);

    // 작업 완료 추적을 위해 외부 atomic 카운터 사용
    std::atomic<uint32_t> completed{ 0 };

    auto start = Clock::now();

    for (uint32_t i = 0; i < jobCount; ++i)
    {
        pool.Submit([&completed]
        {
            // 아주 가벼운 작업 — notify 오버헤드를 더 잘 드러내기 위해 의도적으로 짧게
            volatile int x = 0;
            for (int j = 0; j < 100; ++j) x += j;
            (void)x;
            ++completed;
        });
    }

    // 모든 작업 완료 대기
    while (completed.load() < jobCount)
        std::this_thread::sleep_for(std::chrono::microseconds(10));

    Duration elapsed = Clock::now() - start;

    const uint64_t total   = pool.stats.totalWakeups.load();
    const uint64_t wasted  = pool.stats.wastedWakeups.load();
    const uint64_t useful  = total - wasted;

    std::cout << "\n  [" << label << "]\n";
    std::cout << "  소요 시간        : " << std::fixed << std::setprecision(2)
              << elapsed.count() << " ms\n";
    std::cout << "  총 wakeup 횟수  : " << total << "\n";
    std::cout << "  유효 wakeup     : " << useful
              << "  (작업 처리 성공)\n";
    std::cout << "  낭비 wakeup     : " << wasted
              << "  (깨어났지만 큐 비어 다시 잠듦)\n";
    std::cout << "  wakeup 효율     : "
              << std::fixed << std::setprecision(1)
              << (total > 0 ? (double)useful / total * 100.0 : 0.0) << "%\n";
    std::cout << "  처리 작업 수    : " << completed.load() << " / " << jobCount << "\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    const uint32_t kThreadCount = std::min(8u, std::thread::hardware_concurrency());
    const uint32_t kJobCount    = 10'000;

    std::cout << "=====================================================\n";
    std::cout << "  Day 02 — notify_one vs notify_all 비교\n";
    std::cout << "=====================================================\n";
    std::cout << "  워커 스레드 수: " << kThreadCount << "\n";
    std::cout << "  제출 작업 수:   " << kJobCount << "\n";

    std::cout << "\n-----------------------------------------------------\n";
    std::cout << "  Round 1: 작업 1개당 notify 호출\n";
    std::cout << "-----------------------------------------------------";

    RunExperiment("notify_all (Thundering Herd)", true,  kThreadCount, kJobCount);
    RunExperiment("notify_one (권장)",            false, kThreadCount, kJobCount);

    // ─────────────────────────────────────────────────────────────────────────
    // [결론 안내]
    //
    // notify_all이 맞는 상황:
    //   1. 종료(shutdown) — 모든 스레드가 종료 신호를 봐야 할 때
    //   2. 조건이 여러 스레드 모두에게 의미 있을 때
    //      예) "설정이 바뀌었으니 모두 갱신해라"
    //
    // notify_one이 맞는 상황:
    //   1. 작업 큐에 작업 1개를 넣을 때 — 처리할 스레드도 1개면 충분
    //   2. 생산자-소비자 패턴의 대부분의 경우
    //
    // ThreadPool의 Submit에서 notify_one을 쓰고
    // 소멸자에서 notify_all을 쓰는 이유가 바로 이것이다.
    // ─────────────────────────────────────────────────────────────────────────

    std::cout << "\n=====================================================\n";
    std::cout << "  결론\n";
    std::cout << "=====================================================\n";
    std::cout << "  notify_all: 스레드 " << kThreadCount << "개 전부 깨어남\n";
    std::cout << "              작업 1개 → " << kThreadCount - 1
              << "개는 다시 잠듦 (컨텍스트 스위칭 낭비)\n\n";
    std::cout << "  notify_one: 스레드 1개만 깨어남\n";
    std::cout << "              작업 1개 → 딱 1개가 처리 (낭비 없음)\n\n";
    std::cout << "  ThreadPool.cpp 의 Submit → notify_one\n";
    std::cout << "  ThreadPool.cpp 의 ~ThreadPool → notify_all  ← 이유 이해됐지?\n";
    std::cout << "=====================================================\n\n";

    return 0;
}