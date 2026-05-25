cmake_minimum_required(VERSION 3.17)
project(JobSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(JobSystem
    src/ThreadPool.h
    src/ThreadPool.cpp
    src/main.cpp
)

# 멀티스레딩 라이브러리 링크 (Linux/macOS에서 필요, Windows는 자동)
find_package(Threads REQUIRED)
target_link_libraries(JobSystem PRIVATE Threads::Threads)

# 경고 레벨 설정
if (MSVC)
    target_compile_options(JobSystem PRIVATE /W4)
else()
    target_compile_options(JobSystem PRIVATE -Wall -Wextra -pthread)
endif()