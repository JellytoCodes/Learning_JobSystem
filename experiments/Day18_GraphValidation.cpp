// =============================================================================
// Day 18 — Graph Validation / Topological Sort
//
// 목표:
//   Job Graph를 실행하기 전에 잘못된 dependency를 검증한다.
//
// 검증 항목:
//   - 존재하지 않는 JobId
//   - 자기 자신에 대한 의존성
//   - 중복 dependency
//   - cycle
//
// 유효한 그래프는 topological order를 만든 뒤 ThreadPool에 제출한다.
// =============================================================================

#include "ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

using Ms = std::chrono::milliseconds;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

enum class DependencyPolicy
{
    Completion,
    AllSucceeded
};

struct GraphValidationResult
{
    std::vector<std::string> errors;
    std::vector<size_t> topologicalOrder;

    bool IsValid() const { return errors.empty(); }
};

class GraphValidationException : public std::runtime_error
{
public:
    explicit GraphValidationException(const std::string& message)
        : std::runtime_error(message) {}
};

class ValidatedJobGraph
{
public:
    using JobId = size_t;

    JobId AddJob(
        std::string name,
        std::function<void()> job,
        DependencyPolicy policy = DependencyPolicy::Completion)
    {
        _nodes.push_back(Node{
            std::move(name),
            std::move(job),
            policy
        });
        return _nodes.size() - 1;
    }

    // 의존성 검사는 Validate()까지 지연한다.
    // 덕분에 모든 노드를 먼저 선언한 뒤 forward reference 형태로 연결할 수 있다.
    void AddDependency(JobId job, JobId dependency)
    {
        _edges.push_back({ job, dependency });
    }

    GraphValidationResult Validate() const
    {
        GraphValidationResult result;
        std::vector<std::vector<JobId>> dependencies(_nodes.size());

        ValidateAndBuildAdjacency(dependencies, result.errors);
        DetectCyclesAndBuildOrder(dependencies, result);
        return result;
    }

    std::vector<JobHandle> Run(ThreadPool& pool) const
    {
        const GraphValidationResult validation = Validate();
        if (!validation.IsValid())
            throw GraphValidationException(JoinErrors(validation.errors));

        std::vector<std::vector<JobId>> dependencies(_nodes.size());
        std::vector<std::string> ignoredErrors;
        ValidateAndBuildAdjacency(dependencies, ignoredErrors);

        std::vector<JobHandle> handles(_nodes.size());
        for (JobId id : validation.topologicalOrder)
        {
            const Node& node = _nodes[id];
            std::vector<JobHandle> dependencyHandles;
            dependencyHandles.reserve(dependencies[id].size());

            for (JobId dependency : dependencies[id])
                dependencyHandles.push_back(handles[dependency]);

            if (dependencyHandles.empty())
            {
                handles[id] = pool.Submit(node.job);
            }
            else if (node.policy == DependencyPolicy::AllSucceeded)
            {
                handles[id] = pool.SubmitAfterAllSucceeded(dependencyHandles, node.job);
            }
            else
            {
                handles[id] = pool.SubmitAfter(dependencyHandles, node.job);
            }
        }

        return handles;
    }

    const std::string& GetName(JobId id) const
    {
        if (id >= _nodes.size())
            throw std::out_of_range("ValidatedJobGraph: job id 범위 초과");
        return _nodes[id].name;
    }

private:
    struct Node
    {
        std::string name;
        std::function<void()> job;
        DependencyPolicy policy;
    };

    struct Edge
    {
        JobId job;
        JobId dependency;
    };

    enum class VisitState
    {
        Unvisited,
        Visiting,
        Visited
    };

    void ValidateAndBuildAdjacency(
        std::vector<std::vector<JobId>>& dependencies,
        std::vector<std::string>& errors) const
    {
        std::vector<std::unordered_set<JobId>> seen(_nodes.size());

        for (const Edge& edge : _edges)
        {
            if (edge.job >= _nodes.size())
            {
                errors.push_back("존재하지 않는 job id: " + std::to_string(edge.job));
                continue;
            }

            if (edge.dependency >= _nodes.size())
            {
                errors.push_back(
                    "[" + _nodes[edge.job].name + "] 존재하지 않는 dependency id: "
                    + std::to_string(edge.dependency));
                continue;
            }

            if (edge.job == edge.dependency)
            {
                errors.push_back(
                    "[" + _nodes[edge.job].name + "] 자기 자신을 dependency로 가질 수 없습니다.");
                continue;
            }

            if (!seen[edge.job].insert(edge.dependency).second)
            {
                errors.push_back(
                    "[" + _nodes[edge.job].name + "] dependency 중복: "
                    + _nodes[edge.dependency].name);
                continue;
            }

            dependencies[edge.job].push_back(edge.dependency);
        }
    }

    void DetectCyclesAndBuildOrder(
        const std::vector<std::vector<JobId>>& dependencies,
        GraphValidationResult& result) const
    {
        std::vector<VisitState> states(_nodes.size(), VisitState::Unvisited);
        std::vector<JobId> stack;
        bool cycleFound = false;

        std::function<void(JobId)> visit = [&](JobId id)
        {
            states[id] = VisitState::Visiting;
            stack.push_back(id);

            for (JobId dependency : dependencies[id])
            {
                if (states[dependency] == VisitState::Unvisited)
                {
                    visit(dependency);
                }
                else if (states[dependency] == VisitState::Visiting)
                {
                    auto cycleStart = std::find(stack.begin(), stack.end(), dependency);
                    std::ostringstream message;
                    message << "cycle 발견: ";

                    for (auto it = cycleStart; it != stack.end(); ++it)
                        message << _nodes[*it].name << " -> ";
                    message << _nodes[dependency].name;

                    result.errors.push_back(message.str());
                    cycleFound = true;
                }
            }

            stack.pop_back();
            states[id] = VisitState::Visited;
            result.topologicalOrder.push_back(id);
        };

        for (JobId id = 0; id < _nodes.size(); ++id)
        {
            if (states[id] == VisitState::Unvisited)
                visit(id);
        }

        if (cycleFound)
            result.topologicalOrder.clear();
    }

    static std::string JoinErrors(const std::vector<std::string>& errors)
    {
        std::ostringstream message;
        message << "Job Graph validation 실패:";
        for (const std::string& error : errors)
            message << "\n  - " << error;
        return message.str();
    }

    std::vector<Node> _nodes;
    std::vector<Edge> _edges;
};

static void PrintValidation(const GraphValidationResult& result)
{
    std::cout << "  valid = " << std::boolalpha << result.IsValid() << "\n";
    for (const std::string& error : result.errors)
        std::cout << "  - " << error << "\n";
}

// =============================================================================
// 실험 1: 선언 순서와 무관한 유효 그래프
// =============================================================================
static void Experiment1_ValidGraphAndTopologicalOrder()
{
    std::cout << "\n[실험 1] 유효 그래프 + topological order\n";
    std::cout << "---------------------------------------------\n";

    ThreadPool pool(3);
    ValidatedJobGraph graph;
    std::vector<std::string> log;
    std::mutex logMutex;

    auto Log = [&](const std::string& text)
    {
        std::unique_lock<std::mutex> lock(logMutex);
        log.push_back(text);
    };

    // 실행 순서와 반대로 노드를 먼저 선언해도 dependency 연결 후 정렬할 수 있다.
    auto package = graph.AddJob("Package", [&Log] { Log("Package 완료"); });
    auto compile = graph.AddJob("Compile", [&Log]
    {
        SleepMs(20);
        Log("Compile 완료");
    });
    auto load = graph.AddJob("Load", [&Log]
    {
        SleepMs(20);
        Log("Load 완료");
    });

    graph.AddDependency(package, compile);
    graph.AddDependency(compile, load);

    const auto validation = graph.Validate();
    PrintValidation(validation);

    std::cout << "  제출 순서: ";
    for (auto id : validation.topologicalOrder)
        std::cout << graph.GetName(id) << " ";
    std::cout << "\n";

    auto handles = graph.Run(pool);
    handles[package].Wait();

    std::cout << "  실행 로그: ";
    for (const auto& line : log)
        std::cout << line << " | ";
    std::cout << "\n";
}

// =============================================================================
// 실험 2: invalid id / self dependency / duplicate dependency
// =============================================================================
static void Experiment2_StructuralErrors()
{
    std::cout << "\n[실험 2] 구조 오류 진단\n";
    std::cout << "---------------------------------------------\n";

    ValidatedJobGraph graph;
    auto a = graph.AddJob("A", [] {});
    auto b = graph.AddJob("B", [] {});

    graph.AddDependency(a, a);
    graph.AddDependency(b, a);
    graph.AddDependency(b, a);
    graph.AddDependency(b, 999);
    graph.AddDependency(777, a);

    const auto validation = graph.Validate();
    PrintValidation(validation);

    try
    {
        ThreadPool pool(2);
        (void)graph.Run(pool);
    }
    catch (const GraphValidationException& e)
    {
        std::cout << "  Run 차단 확인: " << e.what() << "\n";
    }
}

// =============================================================================
// 실험 3: cycle path 진단
// =============================================================================
static void Experiment3_CycleDetection()
{
    std::cout << "\n[실험 3] cycle detection\n";
    std::cout << "---------------------------------------------\n";

    ValidatedJobGraph graph;
    auto a = graph.AddJob("A", [] {});
    auto b = graph.AddJob("B", [] {});
    auto c = graph.AddJob("C", [] {});

    graph.AddDependency(a, b);
    graph.AddDependency(b, c);
    graph.AddDependency(c, a);

    const auto validation = graph.Validate();
    PrintValidation(validation);
    std::cout << "  핵심: cycle은 어떤 노드도 안전한 시작점이 없으므로 실행 전 거부\n";
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=====================================================\n";
    std::cout << "  Day 18 — Graph Validation / Topological Sort\n";
    std::cout << "=====================================================\n";

    Experiment1_ValidGraphAndTopologicalOrder();
    Experiment2_StructuralErrors();
    Experiment3_CycleDetection();

    std::cout << "\n=====================================================\n";
    std::cout << "  오늘의 핵심\n";
    std::cout << "=====================================================\n";
    std::cout << "  Validate              : 실행 전에 그래프 구조 오류 진단\n";
    std::cout << "  DFS 3-color           : Visiting 노드 재방문으로 cycle 탐지\n";
    std::cout << "  topological order     : dependency가 먼저 제출되는 순서 생성\n";
    std::cout << "  fail fast             : 잘못된 그래프를 ThreadPool에 제출하지 않음\n";
    std::cout << "=====================================================\n\n";

    return 0;
}
