cmake_minimum_required(VERSION 3.17)
project(Learning_JobSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 멀티스레딩 패키지 찾기
find_package(Threads REQUIRED)

if (MSVC)
    set(COMMON_COMPILE_OPTIONS /W4 /utf-8)
else()
    set(COMMON_COMPILE_OPTIONS -Wall -Wextra -pthread)
endif()

############################## 1 Weeks

# [Day 02 실험]
add_executable(Day02_Notify
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day02_NotifyComparison.cpp
)
target_include_directories(Day02_Notify PRIVATE src)
target_link_libraries(Day02_Notify PRIVATE Threads::Threads)
target_compile_options(Day02_Notify PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 03 실험]
add_executable(Day03_Chunk
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day03_ChunkCount.cpp
)
target_include_directories(Day03_Chunk PRIVATE src)
target_link_libraries(Day03_Chunk PRIVATE Threads::Threads)
target_compile_options(Day03_Chunk PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 04 실험]
add_executable(Day04_Future
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day04_SubmitWithFuture.cpp
)
target_include_directories(Day04_Future PRIVATE src)
target_link_libraries(Day04_Future PRIVATE Threads::Threads)
target_compile_options(Day04_Future PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 05 실험]
add_executable(Day05_PerthreadStats
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day05_PerthreadStats.cpp
)
target_include_directories(Day05_PerthreadStats PRIVATE src)
target_link_libraries(Day05_PerthreadStats PRIVATE Threads::Threads)
target_compile_options(Day05_PerthreadStats PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 06 실험]
add_executable(Day06_DataRace
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day06_DataRace.cpp
)
target_include_directories(Day06_DataRace PRIVATE src)
target_link_libraries(Day06_DataRace PRIVATE Threads::Threads)
target_compile_options(Day06_DataRace PRIVATE ${COMMON_COMPILE_OPTIONS})

############################## 2 Weeks

# [Day 08 실험]
add_executable(Day08_JobHandle
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day08_JobHandle.cpp
)
target_include_directories(Day08_JobHandle PRIVATE src)
target_link_libraries(Day08_JobHandle PRIVATE Threads::Threads)
target_compile_options(Day08_JobHandle PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 09 실험]
add_executable(Day09_SubmitreTurnShandle
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day09_SubmitreTurnShandle.cpp
)
target_include_directories(Day09_SubmitreTurnShandle PRIVATE src)
target_link_libraries(Day09_SubmitreTurnShandle PRIVATE Threads::Threads)
target_compile_options(Day09_SubmitreTurnShandle PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 10 실험]
add_executable(Day10_WorkerWaitStarvation
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day10_WorkerWaitStarvation.cpp
)
target_include_directories(Day10_WorkerWaitStarvation PRIVATE src)
target_link_libraries(Day10_WorkerWaitStarvation PRIVATE Threads::Threads)
target_compile_options(Day10_WorkerWaitStarvation PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 11 실험]
add_executable(Day11_DependencyCounter
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day11_DependencyCounter.cpp
)
target_include_directories(Day11_DependencyCounter PRIVATE src)
target_link_libraries(Day11_DependencyCounter PRIVATE Threads::Threads)
target_compile_options(Day11_DependencyCounter PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 12 실험]
add_executable(Day12_ExceptionPropagation
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day12_ExceptionPropagation.cpp
)
target_include_directories(Day12_ExceptionPropagation PRIVATE src)
target_link_libraries(Day12_ExceptionPropagation PRIVATE Threads::Threads)
target_compile_options(Day12_ExceptionPropagation PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 13 실험]
add_executable(Day13_FailurePolicy
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day13_FailurePolicy.cpp
)
target_include_directories(Day13_FailurePolicy PRIVATE src)
target_link_libraries(Day13_FailurePolicy PRIVATE Threads::Threads)
target_compile_options(Day13_FailurePolicy PRIVATE ${COMMON_COMPILE_OPTIONS})

############################## 3 Weeks

# [Day 15 실험]
add_executable(Day15_HelpingWait
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day15_HelpingWait.cpp
)
target_include_directories(Day15_HelpingWait PRIVATE src)
target_link_libraries(Day15_HelpingWait PRIVATE Threads::Threads)
target_compile_options(Day15_HelpingWait PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 16 실험]
add_executable(Day16_WorkStealing
    experiments/Day16_WorkStealing.cpp
)
target_link_libraries(Day16_WorkStealing PRIVATE Threads::Threads)
target_compile_options(Day16_WorkStealing PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 17 실험]
add_executable(Day17_JobGraphBuilder
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day17_JobGraphBuilder.cpp
)
target_include_directories(Day17_JobGraphBuilder PRIVATE src)
target_link_libraries(Day17_JobGraphBuilder PRIVATE Threads::Threads)
target_compile_options(Day17_JobGraphBuilder PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 18 실험]
add_executable(Day18_GraphValidation
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day18_GraphValidation.cpp
)
target_include_directories(Day18_GraphValidation PRIVATE src)
target_link_libraries(Day18_GraphValidation PRIVATE Threads::Threads)
target_compile_options(Day18_GraphValidation PRIVATE ${COMMON_COMPILE_OPTIONS})

# [Day 19 실험]
add_executable(Day19_GraphExecutionReport
    src/ThreadPool.h
    src/ThreadPool.cpp
    experiments/Day19_GraphExecutionReport.cpp
)
target_include_directories(Day19_GraphExecutionReport PRIVATE src)
target_link_libraries(Day19_GraphExecutionReport PRIVATE Threads::Threads)
target_compile_options(Day19_GraphExecutionReport PRIVATE ${COMMON_COMPILE_OPTIONS})
