cmake_minimum_required(VERSION 3.20)
project(qdev_event_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QDEV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(QDEV_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(QDEV_CALLGRIND "Build the replay-only Callgrind profiling executable" OFF)

add_library(qdev_engine
    src/order_book.cpp
    src/engine.cpp
)
target_include_directories(qdev_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(qdev_engine PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<BOOL:${QDEV_WARNINGS_AS_ERRORS}>:-Werror>
)
if(QDEV_SANITIZE)
    target_compile_options(qdev_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(qdev_engine PUBLIC -fsanitize=address,undefined)
endif()

add_executable(qdev_replay app/replay.cpp)
target_link_libraries(qdev_replay PRIVATE qdev_engine)
target_compile_options(qdev_replay PRIVATE -Wall -Wextra -Wpedantic)

add_executable(qdev_bench benchmark/benchmark.cpp)
target_link_libraries(qdev_bench PRIVATE qdev_engine)
target_compile_options(qdev_bench PRIVATE -Wall -Wextra -Wpedantic)

if(QDEV_CALLGRIND)
    find_path(VALGRIND_INCLUDE_DIR valgrind/callgrind.h REQUIRED)
    add_executable(qdev_profile benchmark/benchmark.cpp)
    target_link_libraries(qdev_profile PRIVATE qdev_engine)
    target_include_directories(qdev_profile PRIVATE ${VALGRIND_INCLUDE_DIR})
    target_compile_definitions(qdev_profile PRIVATE QDEV_CALLGRIND=1)
    target_compile_options(qdev_profile PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(qdev_tests tests/test_engine.cpp)
    target_link_libraries(qdev_tests PRIVATE qdev_engine)
    target_compile_options(qdev_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME qdev_tests COMMAND qdev_tests)

    add_executable(qdev_fill_buffer_tests tests/test_fill_buffer.cpp)
    target_link_libraries(qdev_fill_buffer_tests PRIVATE qdev_engine)
    target_compile_options(qdev_fill_buffer_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME qdev_fill_buffer_tests COMMAND qdev_fill_buffer_tests)
endif()
