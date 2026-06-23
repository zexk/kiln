add_library(kiln_texture STATIC texture.c)
target_include_directories(kiln_texture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_texture PUBLIC kiln_renderer kiln_assets kiln_core)
