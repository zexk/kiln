add_library(kiln_ui STATIC ui.c)
target_include_directories(kiln_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_ui PUBLIC kiln_core kiln_render)
