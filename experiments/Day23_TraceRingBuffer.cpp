// =============================================================================
// Day 23 - Trace Ring Buffer
//
// Goal:
//   Day20 exported a complete trace after graph execution.
//   Day23 tests a fixed-size runtime trace buffer that keeps only the recent
//   event window when the producer writes more events than the buffer can hold.
//
// Key observation:
//   A bounded trace buffer gives predictable memory usage, but old events are
//   overwritten. The exported trace is a recent window, not the whole history.
// =============================================================================

#include "ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Ms = std::chrono::milliseconds;

static void SleepMs(int ms) { std::this_thread::sleep_for(Ms(ms)); }

struct TraceEvent
{
    uint64_t sequence = 0;
    std::string name;
    char phase = 'i';
    TimePoint timestamp{};
    uint64_t threadId = 0;
};

class TraceRingBuffer
{
public:
    explicit TraceRingBuffer(size_t capacity)
        : _events(capacity)
    {
        if (capacity == 0)
            throw std::invalid_argument("TraceRingBuffer capacity must be greater than zero");
    }

    void Record(std::string name, char phase)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        const uint64_t sequence = _nextSequence++;
        TraceEvent& event = _events[sequence % _events.size()];
        event.sequence = sequence;
        event.name = std::move(name);
        event.phase = phase;
        event.timestamp = Clock::now();
        event.threadId = ThreadId();
    }

    std::vector<TraceEvent> Snapshot() const
    {
        std::lock_guard<std::mutex> lock(_mutex);

        const uint64_t total = _nextSequence;
        const uint64_t retained = std::min<uint64_t>(total, _events.size());
        const uint64_t firstSequence = total - retained;

        std::vector<TraceEvent> snapshot;
        snapshot.reserve(static_cast<size_t>(retained));

        for (uint64_t sequence = firstSequence; sequence < total; ++sequence)
        {
            const TraceEvent& event = _events[sequence % _events.size()];
            if (event.sequence == sequence)
                snapshot.push_back(event);
        }

        return snapshot;
    }

    size_t Capacity() const
    {
        return _events.size();
    }

    uint64_t TotalWritten() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _nextSequence;
    }

    uint64_t OverwrittenCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_nextSequence <= _events.size())
            return 0;
        return _nextSequence - _events.size();
    }

private:
    static uint64_t ThreadId()
    {
        return 1ULL +
            static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()) % 900000ULL);
    }

    mutable std::mutex _mutex;
    std::vector<TraceEvent> _events;
    uint64_t _nextSequence = 0;
};

struct SnapshotSummary
{
    uint64_t firstSequence = 0;
    uint64_t lastSequence = 0;
    size_t beginEvents = 0;
    size_t endEvents = 0;
};

static SnapshotSummary Summarize(const std::vector<TraceEvent>& events)
{
    SnapshotSummary summary;
    if (events.empty())
        return summary;

    summary.firstSequence = events.front().sequence;
    summary.lastSequence = events.back().sequence;

    for (const TraceEvent& event : events)
    {
        if (event.phase == 'B')
            ++summary.beginEvents;
        else if (event.phase == 'E')
            ++summary.endEvents;
    }

    return summary;
}

static std::string EscapeJson(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size());

    for (char ch : text)
    {
        switch (ch)
        {
        case '\\': escaped += "\\\\"; break;
        case '"':  escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:   escaped += ch; break;
        }
    }

    return escaped;
}

static int64_t ToMicroseconds(TimePoint origin, TimePoint point)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(point - origin).count();
}

static bool ExportChromeTrace(const std::string& path, const std::vector<TraceEvent>& events)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        return false;

    const TimePoint origin = events.empty() ? Clock::now() : events.front().timestamp;

    output << "{\n  \"displayTimeUnit\": \"ms\",\n  \"traceEvents\": [\n";
    bool first = true;

    auto WriteSeparator = [&]
    {
        if (!first)
            output << ",\n";
        first = false;
    };

    WriteSeparator();
    output << "    {\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":0,"
           << "\"args\":{\"name\":\"Day23 Trace Ring Buffer\"}}";

    for (const TraceEvent& event : events)
    {
        WriteSeparator();
        output << "    {\"name\":\"" << EscapeJson(event.name) << "\","
               << "\"cat\":\"recent_window\",\"ph\":\"" << event.phase << "\","
               << "\"pid\":1,\"tid\":" << event.threadId
               << ",\"ts\":" << ToMicroseconds(origin, event.timestamp)
               << ",\"args\":{\"sequence\":" << event.sequence << "}}";
    }

    output << "\n  ]\n}\n";
    return static_cast<bool>(output);
}

static std::function<void()> TracedJob(
    TraceRingBuffer& buffer,
    std::string name,
    std::function<void()> job)
{
    return [&buffer, name = std::move(name), job = std::move(job)]() mutable
    {
        buffer.Record(name, 'B');
        try
        {
            job();
            buffer.Record(name, 'E');
        }
        catch (...)
        {
            buffer.Record(name, 'E');
            throw;
        }
    };
}

static void PrintSnapshot(
    const std::string& title,
    const TraceRingBuffer& buffer,
    const std::vector<TraceEvent>& snapshot)
{
    const SnapshotSummary summary = Summarize(snapshot);

    std::cout << "\n[" << title << "]\n";
    std::cout << "capacity     : " << buffer.Capacity() << "\n";
    std::cout << "total written: " << buffer.TotalWritten() << "\n";
    std::cout << "retained     : " << snapshot.size() << "\n";
    std::cout << "overwritten  : " << buffer.OverwrittenCount() << "\n";

    if (!snapshot.empty())
    {
        std::cout << "sequence     : " << summary.firstSequence
                  << " .. " << summary.lastSequence << "\n";
        std::cout << "begin/end    : " << summary.beginEvents
                  << " / " << summary.endEvents << "\n";
    }
}

static void Experiment1_BufferLargeEnough()
{
    std::cout << "\n[Experiment 1] buffer large enough keeps every event\n";
    std::cout << "---------------------------------------------------\n";

    ThreadPool pool(4);
    TraceRingBuffer trace(128);

    for (int i = 0; i < 12; ++i)
    {
        (void)pool.Submit(TracedJob(trace, "SmallJob" + std::to_string(i), []
        {
            SleepMs(2);
        }));
    }

    pool.WaitAll();
    PrintSnapshot("full history", trace, trace.Snapshot());
}

static void Experiment2_BufferOverwritesOldEvents()
{
    std::cout << "\n[Experiment 2] fixed buffer keeps only recent events\n";
    std::cout << "---------------------------------------------------\n";

    ThreadPool pool(4);
    TraceRingBuffer trace(20);

    for (int i = 0; i < 48; ++i)
    {
        (void)pool.Submit(TracedJob(trace, "BurstJob" + std::to_string(i), [i]
        {
            SleepMs(1 + (i % 4));
        }));
    }

    pool.WaitAll();
    PrintSnapshot("recent window", trace, trace.Snapshot());
}

static void Experiment3_ExportRecentWindow()
{
    std::cout << "\n[Experiment 3] export recent window as trace JSON\n";
    std::cout << "---------------------------------------------------\n";

    ThreadPool pool(3);
    TraceRingBuffer trace(40);
    std::atomic<int> childCompleted{0};

    (void)pool.Submit(TracedJob(trace, "Parent", [&pool, &trace, &childCompleted]
    {
        for (int i = 0; i < 30; ++i)
        {
            (void)pool.Submit(TracedJob(trace, "Child" + std::to_string(i), [&childCompleted]
            {
                SleepMs(2);
                ++childCompleted;
            }));
        }
    }));

    pool.WaitAll();

    const std::vector<TraceEvent> snapshot = trace.Snapshot();
    PrintSnapshot("exported recent window", trace, snapshot);

    const std::string outputPath = "Day23_RecentTrace.json";
    if (!ExportChromeTrace(outputPath, snapshot))
        throw std::runtime_error("failed to export " + outputPath);

    std::cout << "child completed: " << childCompleted.load() << "\n";
    std::cout << "trace file     : " << outputPath << "\n";
}

int main()
{
    std::cout << "=====================================================\n";
    std::cout << "  Day 23 - Trace Ring Buffer\n";
    std::cout << "=====================================================\n";

    Experiment1_BufferLargeEnough();
    Experiment2_BufferOverwritesOldEvents();
    Experiment3_ExportRecentWindow();

    std::cout << "\nToday takeaway\n";
    std::cout << "  - A trace ring buffer gives fixed memory usage.\n";
    std::cout << "  - When writes exceed capacity, the oldest events are overwritten.\n";
    std::cout << "  - Recent-window trace export is useful, but it is not full history.\n";
    return 0;
}
