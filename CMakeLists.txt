cmake_minimum_required(VERSION 3.20)
project(kiln C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Vulkan REQUIRED)
find_package(X11 REQUIRED)

add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/ecs)
add_subdirectory(src/render)
add_subdirectory(src/assets)
add_subdirectory(src/app)
