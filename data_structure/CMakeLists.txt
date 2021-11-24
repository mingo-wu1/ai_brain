cmake_minimum_required(VERSION 3.5)
project(data_structure)
set(CMAKE_CXX_STANDARD 11)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    set(CMAKE_BUILD_TYPE Debug)
else()
    set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(include)
set(SRCS src/b_tree.cpp)
add_library(${PROJECT_NAME} SHARED ${SRCS})
target_link_libraries(${PROJECT_NAME})