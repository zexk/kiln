add_library(kiln_platform STATIC platform_linux_x11.c)
target_include_directories(kiln_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_platform PUBLIC X11::X11)
