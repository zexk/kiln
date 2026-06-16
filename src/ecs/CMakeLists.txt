add_library(kiln_ecs STATIC ecs.c)
target_include_directories(kiln_ecs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_ecs PUBLIC kiln_core)
