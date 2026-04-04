cmake_minimum_required(VERSION 3.28)
project(url_shortener)

set(CMAKE_CXX_STANDARD 20)

find_package(Boost REQUIRED COMPONENTS json system)

add_executable(server src/server.cpp)

target_link_libraries(server Boost::json Boost::system pthread)
