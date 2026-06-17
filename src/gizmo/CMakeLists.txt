add_library(kiln_gizmo STATIC gizmo.c)
target_include_directories(kiln_gizmo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_gizmo PUBLIC kiln_core kiln_camera kiln_render)
