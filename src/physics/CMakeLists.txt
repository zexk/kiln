add_library(kiln_physics STATIC physics.c)
target_include_directories(kiln_physics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_physics PUBLIC kiln_core)
