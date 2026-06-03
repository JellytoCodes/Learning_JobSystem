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
