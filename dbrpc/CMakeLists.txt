cmake_minimum_required(VERSION 3.16)
project(dbrpc)

set(CMAKE_CXX_STANDARD 14)

add_executable(dbrpc main.cpp bthread.h)