// =============================================================================
// Day 17 — Job Graph Builder
//
// 목표:
//   여러 작업과 의존성을 SubmitAfter 호출 여러 개로 흩어 쓰지 않고,
//   그래프 선언 단계와 실행 단계를 분리하는 사용성 계층을 만든다.
//
// 핵심 아이디어:
//   AddJob(name, dependencies, job, policy)로 노드를 선언하고,
//   Run(pool)을 호출하면 dependency handle을 연결해 ThreadPool에 제출한다.
//
// 빌드 방법 (experiments 폴더에서):
//   g++ -std=c++17 -O2 -pthread ../src/ThreadPool.cpp Day17_JobGraphBuilder.cpp -I../src -o Day17
//   ./Day17
// =============================================================================

#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Ms = std::chrono::milliseconds;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

static void PrintLog(const std::vector<std::string>& log)
{
    for (const auto& line : log)
        std::cout << "  " << line << "\n";
}

enum class DependencyPolicy
{
    Completion,
    AllSucceeded
};

class JobGraphBuilder
{
public:
    using JobId = size_t;

    JobId AddJob(
        std::string name,
        std::vector<JobId> dependencies,
        std::function<void()> job,
        DependencyPolicy policy = DependencyPolicy::Completion)
    {
        for (JobId dependency : dependencies)
        {
            if (dependency >= _nodes.size())
                throw std::invalid_argument("JobGraphBuilder: dependency id가 유효하지 않습니다.");
        }

        _nodes.push_back(Node{
            std::move(name),
            std::move(dependencies),
            std::move(job),
            policy
        });
        return _nodes.size() - 1;
    }

    std::vector<JobHandle> Run(ThreadPool& pool) const
    {
        std::vector<JobHandle> handles(_nodes.size());

        for (JobId id = 0; id < _nodes.size(); ++id)
        {
            const Node& node = _nodes[id];
            std::vector<JobHandle> dependencies;
            dependencies.reserve(node.dependencies.size());

            for (JobId dependencyId : node.dependencies)
                dependencies.push_back(handles[dependencyId]);

            if (dependencies.empty())
            {
                handles[id] = pool.Submit(node.job);
            }
            else if (node.policy == DependencyPolicy::AllSucceeded)
            {
                handles[id] = pool.SubmitAfterAllSucceeded(dependencies, node.job);
            }
            else
            {
                handles[id] = pool.SubmitAfter(dependencies, node.job);
            }
        }

        return handles;
    }

    const std::string& GetName(JobId id) const
    {
        if (id >= _nodes.size())
            throw std::out_of_range("JobGraphBuilder: job id 범위 초과");
        return _nodes[id].name;
    }

    size_t Size() const { return _nodes.size(); }

private:
    struct Node
    {
        std::string name;
        std::vector<JobId> dependencies;
        std::function<void()> job;
        DependencyPolicy policy;
    };

    std::vector<Node> _nodes;
};

static void WaitAllHandles(const JobGraphBuilder& graph, const std::vector<JobHandle>& handles)
{
    for (JobGraphBuilder::JobId id = 0; id < handles.size(); ++id)
    {
        try
        {
            handles[id].Wait();
        }
        catch (const JobCanceledException& e)
        {
            std::cout << "  [" << graph.GetName(id) << "] 취소: " << e.what() << "\n";
        }
        catch (const std::exception& e)
        {
            std::cout << "  [" << graph.GetName(id) << "] 실패: " << e.what() << "\n";
        }
    }
}

// =============================================================================
// 실험 1: 선형 파이프라인 A -> B -> C
// =============================================================================
static void Experiment1_LinearPipeline()
{
    std::cout << "\n[실험 1] 선형 파이프라인 선언\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(3);
    JobGraphBuilder graph;

    std::vector<std::string> log;
    std::mutex logMutex;
    auto Log = [&](const std::string& text)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(text);
    };

    auto load = graph.AddJob("Load", {}, [&Log]
    {
        SleepMs(30);
        Log("Load 완료");
    });

    auto parse = graph.AddJob("Parse", { load }, [&Log]
    {
        SleepMs(30);
        Log("Parse 완료");
    });

    auto build = graph.AddJob("Build", { parse }, [&Log]
    {
        Log("Build 완료");
    });

    auto handles = graph.Run(pool);
    handles[build].Wait();

    PrintLog(log);
    std::cout << "  핵심: SubmitAfter를 직접 나열하지 않고 그래프 선언으로 순서를 표현\n";
}

// =============================================================================
// 실험 2: Fan-out / Fan-in 그래프
// =============================================================================
static void Experiment2_FanOutFanIn()
{
    std::cout << "\n[실험 2] Fan-out / Fan-in 선언\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(4);
    JobGraphBuilder graph;

    constexpr int kParts = 6;
    std::vector<int> partials(kParts, 0);
    std::atomic<int> total{ 0 };

    std::vector<JobGraphBuilder::JobId> parts;
    parts.reserve(kParts);

    for (int i = 0; i < kParts; ++i)
    {
        parts.push_back(graph.AddJob(
            "Part " + std::to_string(i),
            {},
            [i, &partials]
            {
                SleepMs(20 + i * 5);
                partials[i] = (i + 1) * 10;
            }));
    }

    auto sum = graph.AddJob("Sum", parts, [&partials, &total]
    {
        total = std::accumulate(partials.begin(), partials.end(), 0);
    });

    auto handles = graph.Run(pool);
    handles[sum].Wait();

    const int expected = kParts * (kParts + 1) / 2 * 10;
    std::cout << "  노드 수: " << graph.Size() << "\n";
    std::cout << "  합산 결과: " << total.load() << " (예상: " << expected << ")\n";
    std::cout << "  핵심: fan-in 의존성 목록을 그래프 레벨에서 한 번에 확인 가능\n";
}

// =============================================================================
// 실험 3: 그래프 노드별 실패 정책
// =============================================================================
static void Experiment3_PerNodeFailurePolicy()
{
    std::cout << "\n[실험 3] 노드별 실패 정책\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(3);
    JobGraphBuilder graph;

    std::vector<std::string> log;
    std::mutex logMutex;
    auto Log = [&](const std::string& text)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(text);
    };

    auto a = graph.AddJob("A", {}, [&Log]
    {
        Log("A 시작");
        throw std::runtime_error("A 실패");
    });

    auto cleanup = graph.AddJob("Cleanup", { a }, [&Log]
    {
        Log("Cleanup 실행: 완료 기반 정책");
    }, DependencyPolicy::Completion);

    auto product = graph.AddJob("Product", { a }, [&Log]
    {
        Log("Product 실행");
    }, DependencyPolicy::AllSucceeded);

    auto handles = graph.Run(pool);
    WaitAllHandles(graph, handles);

    PrintLog(log);
    std::cout << "  cleanup.IsDone() = " << std::boolalpha << handles[cleanup].IsDone() << "\n";
    std::cout << "  product.HasException() = " << handles[product].HasException() << "\n";
    std::cout << "  핵심: 같은 선행 작업 실패라도 노드 정책에 따라 실행/취소를 분리\n";
}

// =============================================================================
// main
// =============================================================================
int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 17 — Job Graph Builder\n";
    std::cout << "=====================================================\n";

    Experiment1_LinearPipeline();
    Experiment2_FanOutFanIn();
    Experiment3_PerNodeFailurePolicy();

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  JobGraphBuilder       : 작업 선언과 실행을 분리하는 사용성 계층\n";
    std::cout << "  JobId                 : 노드 간 의존성을 핸들 대신 id로 표현\n";
    std::cout << "  DependencyPolicy      : 노드별 완료 기반/성공 기반 실행 정책\n";
    std::cout << "  ThreadPool 코어       : 기존 SubmitAfter 계층을 재사용\n";
    std::cout << "=====================================================\n\n";

    return 0;
}
