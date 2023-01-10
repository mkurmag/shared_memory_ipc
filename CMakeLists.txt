cmake_minimum_required(VERSION 3.12)
project(Proc)

set(CMAKE_CXX_STANDARD 17)

find_package(Boost REQUIRED)

set(PROC_COUNT 3 CACHE STRING "Number of processes")

add_compile_definitions(PROCESSES_COUNT=${PROC_COUNT})

# add_compile_options(-fsanitize=address,undefined)
# add_link_options(-fsanitize=address,undefined)

add_executable(Proc src/main.cpp)
target_link_libraries(Proc PRIVATE Boost::boost)

include_directories(include)

if(TESTS)
    find_package(Catch2 3 REQUIRED)
    add_executable(tests tests/shared_data_container_tests.cpp)
    target_link_libraries(tests PRIVATE Catch2::Catch2WithMain)
    add_custom_target(test ALL COMMAND tests)
endif()
