add_library(kiln_core STATIC core.c arena.c)
target_include_directories(kiln_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
