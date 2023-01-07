cmake_minimum_required(VERSION 3.12)
set(CMAKE_CXX_STANDARD 20)

find_package(Boost REQUIRED)

if(NOT PROC_COUNT)
    set(PROC_COUNT 3)
endif()

add_compile_definitions(PROCESSES_COUNT=${PROC_COUNT})

# add_compile_options(-fsanitize=address,undefined)
# add_link_options(-fsanitize=address,undefined)

project(Proc)

add_executable(Proc src/main.cpp)
unset(PROC_COUNT CACHE)
