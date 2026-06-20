add_library(kiln_camera STATIC camera.c fps_camera.c)
target_include_directories(kiln_camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_camera PUBLIC kiln_core kiln_platform)
