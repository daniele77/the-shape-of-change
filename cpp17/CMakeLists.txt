cmake_minimum_required(VERSION 3.20)

project(RTU VERSION 1.0 LANGUAGES CXX)

# Set C++17 as the required standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(rtu_00 main_00_console.cpp)
add_executable(rtu_01 main_01_value.cpp)
add_executable(rtu_02 main_02_ioitem.cpp)
add_executable(rtu_03 main_03_point.cpp)
add_executable(rtu_04 main_04_cmds.cpp)
add_executable(rtu_05 main_05_alarm.cpp)
add_executable(rtu_06 main_06_proxy.cpp)
add_executable(rtu_07 main_07_factories.cpp)
add_executable(rtu_ai main_ai.cpp)