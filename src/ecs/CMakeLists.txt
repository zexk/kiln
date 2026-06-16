add_library(kiln_ecs STATIC
    archetype.c
    world.c
    query.c
)
target_include_directories(kiln_ecs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_ecs PUBLIC kiln_core)
