// =============================================================================
// Day 01 — Spurious Wakeup 실험
//
// 목표:
//   condition_variable의 predicate 람다가 왜 반드시 필요한지 직접 눈으로 확인.
//
// 실험 구조:
//   [BROKEN]  wait(lock)만 사용 — spurious wakeup 방어 없음
//   [SAFE]    wait(lock, predicate) 사용 — spurious wakeup 자동 방어
//
// 실행 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O0 -pthread Day01_SpuriousWakeup.cpp -o Day01 && ./Day01
//   (최적화 끄기 -O0 권장 — 레이스 컨디션이 더 잘 드러남)
// =============================================================================

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <chrono>
#include <atomic>
#include <string>

// =============================================================================
// 실험용 단순 Producer-Consumer
//
// Producer : 작업을 큐에 넣음
// Consumer : 큐에서 작업을 꺼내 실행
//
// condition_variable이 없으면?
//   Consumer가 큐를 계속 확인(busy-wait) → CPU 100% 점유
//
// condition_variable이 있으면?
//   큐가 비었을 때 Consumer를 재운다 → CPU 낭비 없음
//   근데 여기서 "spurious wakeup" 문제가 생긴다.
// =============================================================================

// ─────────────────────────────────────────────────────────────────────────────
// 공통 상태
// ─────────────────────────────────────────────────────────────────────────────
static std::queue<std::function<void()>> g_queue;
static std::mutex                        g_mutex;
static std::condition_variable           g_cv;
static std::atomic<bool>                 g_stop{ false };
static std::atomic<int>                  g_processedCount{ 0 };
static std::atomic<int>                  g_spuriousCount{ 0 };  // 가짜 깨어남 횟수 측정용

// ─────────────────────────────────────────────────────────────────────────────
// [BROKEN] predicate 없는 Consumer
//
// spurious wakeup이란?
//   OS 스케줄러가 notify 없이도 스레드를 깨울 수 있다.
//   POSIX 표준에 명시된 허용된 동작 (Windows도 동일).
//   드물지만 확실히 발생하며, 발생 빈도는 OS/하드웨어/부하에 따라 다르다.
//
// 아래 코드의 문제:
//   spurious wakeup 시 큐가 비어있는데도 front() 를 호출 → Undefined Behavior
//   운 좋으면 아무 일 없고, 운 나쁘면 크래시 or 잘못된 결과.
// ─────────────────────────────────────────────────────────────────────────────
static void BrokenConsumer()
{
    while (!g_stop)
    {
        std::function<void()> job;

        {
            std::unique_lock<std::mutex> lock(g_mutex);

            // ❌ predicate 없이 그냥 wait.
            //    spurious wakeup 시 큐가 비어 있어도 그냥 통과해버린다.
            g_cv.wait(lock);

            // spurious wakeup이 일어났을 때 큐가 비어 있으면 → 문제 발생 지점.
            if (g_queue.empty())
            {
                // 실제로 이 상황이 얼마나 자주 일어나는지 기록.
                ++g_spuriousCount;
                // 여기서 front()를 호출했다면 UB. 지금은 체크하고 skip.
                continue;
            }

            job = std::move(g_queue.front());
            g_queue.pop();
        }

        if (job) { job(); ++g_processedCount; }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// [SAFE] predicate 있는 Consumer
//
// wait(lock, predicate)의 내부 동작:
//   while (!predicate()) {   ← predicate가 false면 계속 잠든다
//       wait(lock);
//   }
//
//   즉, spurious wakeup으로 깨어나도 predicate를 재확인해서
//   조건이 충족되지 않으면 다시 잠든다. 자동으로 방어된다.
// ─────────────────────────────────────────────────────────────────────────────
static void SafeConsumer()
{
    while (!g_stop)
    {
        std::function<void()> job;

        {
            std::unique_lock<std::mutex> lock(g_mutex);

            // ✅ predicate 포함. spurious wakeup이 와도 큐가 빌 때는 다시 잠든다.
            g_cv.wait(lock, []
                {
                    return !g_queue.empty() || g_stop.load();
                });

            if (g_stop && g_queue.empty()) return;

            job = std::move(g_queue.front());
            g_queue.pop();
        }

        if (job) { job(); ++g_processedCount; }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 실험 실행 헬퍼
// ─────────────────────────────────────────────────────────────────────────────
static void RunExperiment(const std::string& name,
    void (*consumerFn)(),
    int jobCount)
{
    // 상태 초기화
    while (!g_queue.empty()) g_queue.pop();
    g_stop = false;
    g_processedCount = 0;
    g_spuriousCount = 0;

    std::cout << "\n[" << name << "]\n";
    std::cout << "  제출할 작업 수: " << jobCount << "\n";

    // Consumer 스레드 시작
    std::thread consumer(consumerFn);

    // Producer: 짧은 간격으로 작업 제출
    // 간격을 짧게 할수록 spurious wakeup 재현 가능성이 높아진다.
    for (int i = 0; i < jobCount; ++i)
    {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_queue.push([i] {
                // 실제 작업 (단순 출력 억제, 카운트만)
                volatile int x = i * i;
                (void)x;
                });
        }
        g_cv.notify_one();

        // notify 사이에 아주 짧은 간격을 두면 spurious wakeup 가능성 증가
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    // 완료 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 종료
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_cv.notify_all();
    consumer.join();

    std::cout << "  처리된 작업 수: " << g_processedCount.load() << " / " << jobCount << "\n";
    std::cout << "  빈 큐에서 깨어난 횟수 (spurious): " << g_spuriousCount.load() << "\n";

    if (g_processedCount.load() == jobCount)
        std::cout << "  ✅ 결과 정상\n";
    else
        std::cout << "  ❌ 결과 불일치 — " << (jobCount - g_processedCount.load()) << "개 유실\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    constexpr int kJobCount = 500;

    std::cout << "=====================================================\n";
    std::cout << "  Day 01 — Spurious Wakeup 실험\n";
    std::cout << "=====================================================\n";
    std::cout << "\n";
    std::cout << "  [BROKEN] : wait(lock) — predicate 없음\n";
    std::cout << "  [SAFE]   : wait(lock, predicate) — spurious wakeup 방어\n";
    std::cout << "\n";
    std::cout << "  주의: BROKEN 버전이 항상 틀리진 않는다.\n";
    std::cout << "  spurious wakeup은 비결정적(non-deterministic)이므로\n";
    std::cout << "  같은 코드도 실행마다 결과가 다를 수 있다.\n";
    std::cout << "  이게 바로 이런 버그가 찾기 어려운 이유다.\n";

    // ─────────────────────────────────────────────────────────────────────────
    // [핵심 관찰 포인트]
    //
    // BROKEN 버전에서 "빈 큐에서 깨어난 횟수"가 0보다 크면
    // spurious wakeup이 실제로 발생한 것.
    // front()를 그냥 불렀다면 그 순간이 UB 발생 지점이다.
    //
    // SAFE 버전에서는 spurious wakeup이 와도 predicate로 차단되므로
    // 항상 처리된 작업 수 == 제출한 작업 수 가 성립해야 한다.
    // ─────────────────────────────────────────────────────────────────────────

    // 여러 번 반복해서 비결정적 특성 확인
    for (int trial = 1; trial <= 3; ++trial)
    {
        std::cout << "\n--- Trial " << trial << " ---";
        RunExperiment("BROKEN", BrokenConsumer, kJobCount);
        RunExperiment("SAFE", SafeConsumer, kJobCount);
    }

    std::cout << "\n=====================================================\n";
    std::cout << "  결론\n";
    std::cout << "=====================================================\n";
    std::cout << "  wait(lock, pred) 는\n";
    std::cout << "  while (!pred()) { wait(lock); } 과 동일하다.\n\n";
    std::cout << "  predicate는 spurious wakeup을 자동으로 처리하고,\n";
    std::cout << "  종료 조건(_stop)과 실제 조건(큐 비어있지 않음)을\n";
    std::cout << "  한 곳에서 명확하게 표현할 수 있게 해준다.\n";
    std::cout << "=====================================================\n\n";

    return 0;
}