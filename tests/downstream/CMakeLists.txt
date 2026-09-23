# Fabric Efficiency Ledger - independent downstream consumer.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project is deliberately NOT part of the library build. It exists to prove
# that the installed package can be consumed by an unrelated CMake project
# through find_package(fel CONFIG REQUIRED) alone.
cmake_minimum_required(VERSION 3.25)
project(fel_downstream_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fel CONFIG REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE fel::fel)

enable_testing()
add_test(NAME downstream COMMAND downstream_consumer)
