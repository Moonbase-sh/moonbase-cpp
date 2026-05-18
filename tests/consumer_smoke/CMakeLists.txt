cmake_minimum_required(VERSION 3.20)

project(moonbase_cpp_consumer_smoke LANGUAGES CXX)

find_package(moonbase_cpp REQUIRED)

add_executable(moonbase_cpp_consumer_smoke main.cpp)
target_link_libraries(moonbase_cpp_consumer_smoke PRIVATE moonbase::licensing)
