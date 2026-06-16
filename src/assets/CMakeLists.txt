add_library(kiln_assets STATIC assets.c)
target_include_directories(kiln_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_assets PUBLIC kiln_core)
