add_library(kiln_render STATIC render.c)
target_include_directories(kiln_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_render PUBLIC kiln_core Vulkan::Vulkan)
