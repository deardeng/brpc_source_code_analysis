cmake_minimum_required(VERSION 3.16)
project(dbrpc)

set(CMAKE_CXX_STANDARD 14)

add_executable(dbrpc main.cpp bthread.h bthread.cpp task_control.h task_control.cpp task_group.h task_group_inl.h task_group.cpp butex.h butex.cpp)