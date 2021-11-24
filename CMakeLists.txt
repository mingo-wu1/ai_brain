cmake_minimum_required(VERSION 3.5)
project(ai_brain)
set(CMAKE_CXX_STANDARD 11)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    set(CMAKE_BUILD_TYPE Debug)
else()
    set(CMAKE_BUILD_TYPE Release)
endif()

option(USE_DATA_STRUCTURE ON)

message("=============CMake Info===============")
message("CMAKE_MINIMUM_REQUIRED_VERSION:" ${CMAKE_MINIMUM_REQUIRED_VERSION})
message("CMAKE_CXX_STANDARD:" ${CMAKE_CXX_STANDARD})
message("CMAKE_BUILD_TYPE:" ${CMAKE_BUILD_TYPE})
message("USE_DATA_STRUCTURE:" ${USE_DATA_STRUCTURE})
message("======================================")

include_directories()
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME})

if(USE_DATA_STRUCTURE)
add_subdirectory(data_structure)
target_link_libraries(${PROJECT_NAME} data_structure)
endif()