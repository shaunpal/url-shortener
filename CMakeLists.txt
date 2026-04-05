cmake_minimum_required(VERSION 4.0)
project(url_shortener)

set(CMAKE_CXX_STANDARD 20)

# Tell CMake that version 18 is a known version
set(PostgreSQL_ADDITIONAL_VERSIONS "18" "18.3")

# If on Apple Silicon Mac, help CMake find the Homebrew path
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/postgresql@18") # Adjust this path if your postgresql is install on different path

find_package(Boost REQUIRED COMPONENTS json system headers url)
find_package(OpenSSL REQUIRED)
find_package(PostgreSQL REQUIRED)

add_executable(server src/server.cpp)

target_link_libraries(server PRIVATE Boost::json Boost::system Boost::headers Boost::url pthread OpenSSL::SSL OpenSSL::Crypto PostgreSQL::PostgreSQL)
