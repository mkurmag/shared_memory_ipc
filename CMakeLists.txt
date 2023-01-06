cmake_minimum_required(VERSION 3.12)
set(CMAKE_CXX_STANDARD 20)

find_package(Boost REQUIRED)

project(Proc)

add_executable(Proc src/main.cpp)
