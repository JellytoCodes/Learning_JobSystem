#pragma once

#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <vector>
#include <stdexcept>
#include <future>          // std::future, std::packaged_task
#include <memory>          // std::shared_ptr
#include <type_traits>     // std::invoke_result_t

// =============================================================================
// ThreadPool
//
// [핵심 개념]
//   스레드를 미리 N개 만들어두고 재사용하는 구조.
//
//   왜 미리 만드냐?
//     std::thread 생성 비용은 생각보다 크다 (OS 커널 자원 할당, 스택 메모리 등).
//     작업마다 스레드를 새로 만들고 죽이면 오버헤드가 작업 자체보다 커질 수 있다.
//     미리 만들어두면 작업 제출(Submit) 시 즉시 실행 가능하다.
//
//   구조 요약:
//     [메인 스레드]       Submit(job) ──→  [Job Queue]
//                                               ↓ (락 경쟁)
//     [Worker 스레드 0]  ←── job 꺼냄 ──←──────┤
//     [Worker 스레드 1]  ←── job 꺼냄 ──←──────┤
//     [Worker 스레드 2]  ←── job 꺼냄 ──←──────┘
// =============================================================================
class ThreadPool
{
public:
    // -------------------------------------------------------------------------
    // 생성자
    //   threadCount: 생성할 워커 스레드 수.
    //   일반적으로 std::thread::hardware_concurrency() (논리 코어 수) 를 넘기면 된다.
    //   코어 수보다 많이 만들면 컨텍스트 스위칭 비용이 오히려 증가한다.
    // -------------------------------------------------------------------------
    explicit ThreadPool(uint32_t threadCount);

    // -------------------------------------------------------------------------
    // 소멸자
    //   _stop 플래그를 세운 뒤 모든 워커 스레드가 깨끗하게 종료될 때까지 join().
    //   join() 없이 소멸하면 실행 중인 스레드가 댕글링 참조를 건드려 크래시 발생.
    // -------------------------------------------------------------------------
    ~ThreadPool();

    // 복사/이동 금지 — 내부 스레드와 mutex는 이동 불가능한 자원이다.
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&)                 = delete;
    ThreadPool& operator=(ThreadPool&&)      = delete;

    // -------------------------------------------------------------------------
    // Submit
    //   실행할 작업(job)을 큐에 넣는다.
    //   std::function<void()>이므로 람다, 함수 포인터, functor 모두 넣을 수 있다.
    //
    //   예:
    //     pool.Submit([&result, i] { result[i] = HeavyComputation(i); });
    //
    //   Submit 직후 job이 즉시 실행되는 게 아니라,
    //   워커 스레드 중 하나가 큐에서 꺼내 실행한다.
    // -------------------------------------------------------------------------
    void Submit(std::function<void()> job);

    // -------------------------------------------------------------------------
    // SubmitWithFuture
    //   반환값이 있는 작업을 제출하고, std::future<T>를 즉시 반환한다.
    //   future.get()을 호출하면 작업 완료까지 블록한 뒤 결과를 반환한다.
    //
    //   Submit과의 차이:
    //     Submit      : 결과를 외부 배열에 직접 써야 함 (index 관리 필요)
    //     SubmitWithFuture : future가 결과를 들고 다님 (index 불필요)
    //
    //   예:
    //     auto f = pool.SubmitWithFuture([] { return ComputeSomething(); });
    //     // ... 다른 작업 수행 ...
    //     int result = f.get();  // 완료될 때까지 여기서 블록
    //
    //   예외 처리:
    //     작업 내부에서 예외가 발생하면 future에 저장된다.
    //     f.get() 호출 시 그 예외가 다시 던져진다.
    //
    //   [구현 핵심] packaged_task를 shared_ptr로 감싸는 이유:
    //     std::packaged_task는 복사 불가(move-only) 타입이다.
    //     하지만 Submit에 넘기는 std::function<void()>은 복사 가능해야 한다.
    //     shared_ptr로 감싸면 람다가 포인터(복사 가능)를 캡처하므로 해결된다.
    // -------------------------------------------------------------------------
    template<typename Func>
    auto SubmitWithFuture(Func&& func) -> std::future<std::invoke_result_t<Func>>
    {
        using ReturnType = std::invoke_result_t<Func>;

        // packaged_task: func을 감싸고, 연결된 future를 제공한다.
        // task를 실행하면 → future가 결과(또는 예외)를 받는다.
        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::forward<Func>(func)
        );

        // future를 미리 꺼낸다. task가 사라져도 future는 독립적으로 살아남는다.
        std::future<ReturnType> future = task->get_future();

        // task의 복사본이 아닌 shared_ptr을 캡처 → packaged_task 복사 문제 해결
        Submit([task]() { (*task)(); });

        return future;
    }

    // -------------------------------------------------------------------------
    // WaitAll
    //   제출된 모든 작업이 완료될 때까지 호출한 스레드(메인)를 블록한다.
    //   내부적으로 _pendingJobs 카운터가 0이 될 때까지 condition_variable로 대기.
    //
    //   주의: WaitAll 중에도 다른 스레드에서 Submit을 하면 그 작업까지 기다린다.
    // -------------------------------------------------------------------------
    void WaitAll();

    // -------------------------------------------------------------------------
    // GetThreadCount
    //   현재 생성된 워커 스레드 수 반환.
    // -------------------------------------------------------------------------
    uint32_t GetThreadCount() const { return static_cast<uint32_t>(_workers.size()); }

    // -------------------------------------------------------------------------
    // GetPendingJobCount
    //   아직 완료되지 않은 작업 수 (실행 중 + 큐 대기 중 합산).
    // -------------------------------------------------------------------------
    uint32_t GetPendingJobCount() const { return _pendingJobs.load(); }

    // -------------------------------------------------------------------------
    // ThreadStats / GetPerThreadStats
    //   각 워커 스레드가 처리한 작업 수를 반환한다.
    //   로드 밸런싱이 잘 되고 있는지 확인하는 용도.
    //   이상적으로는 모든 스레드의 jobsProcessed가 비슷해야 한다.
    // -------------------------------------------------------------------------
    struct ThreadStats
    {
        uint32_t threadIdx;
        uint64_t jobsProcessed;
    };

    std::vector<ThreadStats> GetPerThreadStats() const
    {
        const uint32_t n = GetThreadCount();
        std::vector<ThreadStats> stats;
        stats.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
            stats.push_back({ i, _perThreadJobCount[i].load() });
        return stats;
    }

    void ResetStats()
    {
        for (uint32_t i = 0; i < GetThreadCount(); ++i)
            _perThreadJobCount[i].store(0);
    }

private:
    // -------------------------------------------------------------------------
    // WorkerLoop
    //   threadIdx: 이 스레드의 고유 인덱스 (통계 기록용).
    // -------------------------------------------------------------------------
    void WorkerLoop(uint32_t threadIdx);

private:
    // 워커 스레드 목록
    // emplace_back으로 한 번만 채우고 이후 수정하지 않으므로 별도 락 불필요.
    std::vector<std::thread> _workers;

    // Job 큐
    // 여러 스레드가 동시에 접근하므로 반드시 _queueMutex로 보호해야 한다.
    std::queue<std::function<void()>> _jobQueue;

    // _jobQueue 접근을 직렬화(serialize)하는 뮤텍스.
    // mutex는 한 번에 하나의 스레드만 진입 허용 → 데이터 레이스 방지.
    std::mutex _queueMutex;

    // -------------------------------------------------------------------------
    // condition_variable
    //   큐가 비었을 때 워커 스레드를 잠재우고, 작업이 들어오면 깨우는 도구.
    //
    //   왜 필요한가?
    //     잠재우지 않으면 워커들이 빈 큐를 계속 확인(busy-wait)하며 CPU를 100% 점유.
    //     condition_variable은 OS 레벨에서 스레드를 블록하므로 CPU를 낭비하지 않는다.
    //
    //   wait()   : 뮤텍스를 원자적으로 해제 + 스레드 블록 (다른 스레드가 뮤텍스 취득 가능)
    //   notify_one() : 잠든 스레드 하나만 깨움 (Submit 시 사용)
    //   notify_all() : 잠든 모든 스레드 깨움 (종료 시 사용)
    // -------------------------------------------------------------------------
    std::condition_variable _workerCv;

    // WaitAll()에서 완료를 기다리기 위한 별도 CV + 뮤텍스
    // _queueMutex와 분리하는 이유: WaitAll 대기 중에도 Submit이 가능해야 하기 때문.
    std::mutex              _completionMutex;
    std::condition_variable _completionCv;

    // -------------------------------------------------------------------------
    // _stop
    //   소멸자에서 true로 세팅. 워커 스레드들이 이 플래그를 보고 루프 탈출.
    //   std::atomic이므로 락 없이 안전하게 읽고 쓸 수 있다.
    // -------------------------------------------------------------------------
    std::atomic<bool> _stop{ false };

    // -------------------------------------------------------------------------
    // _pendingJobs
    //   Submit 시 ++, 작업 완료 시 --. WaitAll이 0이 될 때까지 기다린다.
    //   std::atomic이므로 여러 스레드에서 동시에 증감해도 안전하다.
    //
    //   왜 atomic인가?
    //     일반 int를 여러 스레드가 동시에 읽고/쓰면 데이터 레이스(UB) 발생.
    //     atomic은 CPU 명령어 수준에서 원자적 연산을 보장한다.
    // -------------------------------------------------------------------------
    std::atomic<uint32_t> _pendingJobs{ 0 };

    // -------------------------------------------------------------------------
    // _perThreadJobCount
    //   스레드별 처리 작업 수. WorkerLoop에서 작업 완료 시 해당 인덱스 ++.
    //   각 스레드는 자신의 인덱스에만 접근하므로 락 불필요.
    //
    //   왜 vector가 아닌 unique_ptr<atomic[]>인가?
    //     std::atomic은 복사/이동 불가 타입이다.
    //     vector는 내부 재할당 시 원소를 이동시키므로 atomic과 함께 쓸 수 없다.
    //     unique_ptr<T[]>는 고정 크기 배열을 힙에 한 번 할당하고 이동하지 않는다.
    //     make_unique<atomic<uint64_t>[]>(n)은 모든 원소를 0으로 value-init한다.
    // -------------------------------------------------------------------------
    std::unique_ptr<std::atomic<uint64_t>[]> _perThreadJobCount;
};