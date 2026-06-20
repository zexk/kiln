add_library(kiln_core STATIC core.c arena.c linalg.c mesh.c fs.c
    timer.c aabb.c frustum.c noise.c log.c input.c)
target_include_directories(kiln_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_core PUBLIC m)
