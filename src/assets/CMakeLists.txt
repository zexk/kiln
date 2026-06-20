add_library(kiln_assets STATIC assets.c obj.c image.c scene.c kmesh.c)
target_include_directories(kiln_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_assets PUBLIC kiln_core)

# stb_image.h's implementation doesn't pass our -Wall -Wextra; silence just it.
set_source_files_properties(image.c PROPERTIES COMPILE_FLAGS "-w")
