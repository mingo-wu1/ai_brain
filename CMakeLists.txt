cmake_minimum_required(VERSION 3.5)
project(ai_brain)
set(CMAKE_CXX_STANDARD 11)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    set(CMAKE_BUILD_TYPE Debug)
else()
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_DATA_STRUCTURE "build data structure" TRUE)
option(BUILD_LIVE_SERVE "build live serve" TRUE)

message("=============CMake Info===============")
message("CMAKE_MINIMUM_REQUIRED_VERSION:" ${CMAKE_MINIMUM_REQUIRED_VERSION})
message("CMAKE_CXX_STANDARD:" ${CMAKE_CXX_STANDARD})
message("CMAKE_BUILD_TYPE:" ${CMAKE_BUILD_TYPE})
message("BUILD_DATA_STRUCTURE:" ${BUILD_DATA_STRUCTURE})
message("BUILD_LIVE_SERVE:" ${BUILD_LIVE_SERVE})
message("======================================")

include_directories()
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME})
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)

if(BUILD_DATA_STRUCTURE)
add_subdirectory(data_structure)
target_link_libraries(${PROJECT_NAME} data_structure)
endif()

if(BUILD_LIVE_SERVE)
add_subdirectory(live_serve/ZLMediaKit)
endif()