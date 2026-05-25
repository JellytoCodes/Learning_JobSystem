#include "ThreadPool.h"

// =============================================================================
// 생성자
// =============================================================================
ThreadPool::ThreadPool(uint32_t threadCount)
{
    if (threadCount == 0)
        throw std::invalid_argument("ThreadPool: threadCount는 1 이상이어야 합니다.");

    // 워커 스레드를 threadCount개 생성.
    // 각 스레드는 생성 즉시 WorkerLoop()를 실행하며 작업을 기다린다.
    // reserve로 메모리를 먼저 확보하지 않으면, emplace_back 중 vector 재할당 시
    // 이미 시작된 thread 객체가 이동되어 크래시가 발생할 수 있다.
    _workers.reserve(threadCount);
    for (uint32_t i = 0; i < threadCount; ++i)
    {
        // [this]로 ThreadPool 인스턴스를 캡처해 WorkerLoop 실행.
        // 스레드 생성 시점부터 WorkerLoop가 돌기 시작하며, 큐가 비어 있으므로
        // 곧바로 condition_variable에서 잠든다.
        _workers.emplace_back([this] { WorkerLoop(); });
    }
}

// =============================================================================
// 소멸자
// =============================================================================
ThreadPool::~ThreadPool()
{
    // -------------------------------------------------------------------------
    // Step 1: _stop 플래그를 세운다.
    //   락을 잡고 세우는 이유:
    //     _stop = true 와 notify_all() 사이에 워커가 wait()에서 빠져나와
    //     다시 잠들어 버리면 종료 신호를 놓칠 수 있다.
    //     락 안에서 _stop을 세워야 이 타이밍 이슈(race condition)를 막을 수 있다.
    // -------------------------------------------------------------------------
    {
        std::unique_lock<std::mutex> lock(_queueMutex);
        _stop = true;
    }

    // Step 2: 잠든 모든 워커 스레드를 깨운다.
    // 깨어난 스레드들은 WorkerLoop에서 _stop 조건을 확인하고 루프를 탈출한다.
    _workerCv.notify_all();

    // Step 3: 모든 스레드가 완전히 종료될 때까지 기다린다.
    // join()을 하지 않으면 스레드가 실행 중인데 ThreadPool이 소멸해 버려
    // _queueMutex, _workerCv 등 멤버 변수에 접근하다가 크래시가 발생한다.
    for (auto& worker : _workers)
        worker.join();
}

// =============================================================================
// Submit — 작업을 큐에 넣는다
// =============================================================================
void ThreadPool::Submit(std::function<void()> job)
{
    {
        std::unique_lock<std::mutex> lock(_queueMutex);

        // 소멸 중인 풀에 작업을 제출하는 것은 논리적 오류.
        if (_stop)
            throw std::runtime_error("ThreadPool: 종료된 풀에 작업을 제출할 수 없습니다.");

        // std::move로 job을 큐에 넣는다.
        // 복사 대신 이동을 써야 std::function 내부 캡처된 데이터가 복사되지 않는다.
        _jobQueue.push(std::move(job));

        // 제출된 작업 수 증가.
        // ++는 락 안에서 수행 — _pendingJobs와 큐 상태가 항상 일치하도록 보장.
        ++_pendingJobs;
    }

    // 락을 해제한 뒤 notify.
    // 락을 잡은 채로 notify하면 깨어난 스레드가 즉시 락을 얻으려다 다시 블록된다.
    // 불필요한 컨텍스트 스위칭을 줄이기 위해 락 해제 후에 notify하는 것이 관례.
    _workerCv.notify_one();
}

// =============================================================================
// WaitAll — 모든 작업이 완료될 때까지 대기
// =============================================================================
void ThreadPool::WaitAll()
{
    std::unique_lock<std::mutex> lock(_completionMutex);

    // _pendingJobs가 0이 될 때까지 블록.
    // 람다 predicate를 쓰는 이유:
    //   condition_variable은 spurious wakeup(가짜 깨어남)이 발생할 수 있다.
    //   OS 스케줄러가 이유 없이 스레드를 깨울 수 있기 때문.
    //   predicate가 있으면 wait()가 내부에서 조건을 재확인하고,
    //   false면 다시 잠든다 → spurious wakeup을 자동으로 처리.
    _completionCv.wait(lock, [this] { return _pendingJobs.load() == 0; });
}

// =============================================================================
// WorkerLoop — 각 워커 스레드가 실행하는 루프
// =============================================================================
void ThreadPool::WorkerLoop()
{
    // 스레드가 종료 신호를 받을 때까지 무한 반복.
    while (true)
    {
        std::function<void()> job;

        // ------------------------------------------------------------------
        // 임계 구역(Critical Section): 큐에서 작업 하나를 꺼낸다.
        // ------------------------------------------------------------------
        {
            std::unique_lock<std::mutex> lock(_queueMutex);

            // 큐가 비어 있고 종료 신호도 없으면 잠든다.
            //
            // wait()의 내부 동작:
            //   1. 뮤텍스를 원자적으로 해제 (다른 스레드가 Submit 가능해짐)
            //   2. 스레드를 블록 (CPU 점유 없음)
            //   3. notify가 오면 뮤텍스를 다시 획득하고 predicate 재확인
            //   4. predicate가 true면 wait 탈출, false면 다시 잠든다
            _workerCv.wait(lock, [this]
            {
                return !_jobQueue.empty() || _stop;
            });

            // 종료 신호를 받았고 큐도 비었으면 루프 탈출 → 스레드 종료.
            // _stop만 확인하면 큐에 남은 작업들이 처리되지 않는다.
            // 큐가 빌 때까지 남은 작업을 마저 처리하는 것이 graceful shutdown의 원칙.
            if (_stop && _jobQueue.empty())
                return;

            // 큐 앞에서 작업 하나를 이동(move)으로 꺼낸다.
            // 복사 대신 이동을 써서 std::function 내부 데이터 복사 비용을 없앤다.
            job = std::move(_jobQueue.front());
            _jobQueue.pop();

        } // 여기서 락 해제 → 다른 워커가 동시에 큐에 접근 가능해진다.

        // ------------------------------------------------------------------
        // 임계 구역 밖에서 작업 실행.
        // 락을 잡은 채로 실행하면 다른 스레드가 큐에 접근하지 못해
        // 병렬성이 완전히 사라진다. 반드시 락 해제 후에 실행해야 한다.
        // ------------------------------------------------------------------
        job();

        // ------------------------------------------------------------------
        // 작업 완료 처리
        // ------------------------------------------------------------------

        // 완료된 작업 수 차감. fetch_sub는 원자적 감소 연산.
        // 반환값은 감소 이전의 값이므로, 1이었으면 현재 0이 된 것.
        const uint32_t remaining = _pendingJobs.fetch_sub(1) - 1;

        // 마지막 작업이 완료됐으면 WaitAll()을 깨운다.
        if (remaining == 0)
        {
            std::unique_lock<std::mutex> lock(_completionMutex);
            _completionCv.notify_all();
        }
    }
}