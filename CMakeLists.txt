cmake_minimum_required(VERSION 3.20)
project(vote_secure LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
add_subdirectory(cppcoro)

add_executable(vote_secure main.cpp mpc_service.cpp mpc_service.h utils.cpp utils.h talliers_network.cpp talliers_network.h endian_number.h exchange_item.h)
target_link_libraries(vote_secure PRIVATE cppcoro)
