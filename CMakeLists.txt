cmake_minimum_required(VERSION 3.5)
project(ai_brain)
set(CMAKE_CXX_STANDARD 11)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    set(CMAKE_BUILD_TYPE Debug)
else()
    set(CMAKE_BUILD_TYPE Release)
endif()

message("=============CMake Info===============")
message("CMAKE_MINIMUM_REQUIRED_VERSION:" ${CMAKE_MINIMUM_REQUIRED_VERSION})
message("CMAKE_CXX_STANDARD:" ${CMAKE_CXX_STANDARD})
message("CMAKE_BUILD_TYPE:" ${CMAKE_BUILD_TYPE})
message("======================================")

include_directories()
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME})

