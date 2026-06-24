add_library(kiln_voxel STATIC
    kv_block.c
    kv_mesh.c
    kv_world.c)

target_include_directories(kiln_voxel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_voxel PUBLIC
    kiln_core
    kiln_renderer
    kiln_texture)
