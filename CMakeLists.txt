cmake_minimum_required(VERSION 3.17)
project(Learning_JobSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 멀티스레딩 패키지 찾기
find_package(Threads REQUIRED)

if (MSVC)
    set(COMMON_COMPILE_OPTIONS /W4)
else()
    set(COMMON_COMPILE_OPTIONS -Wall -Wextra -pthread)
endif()

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