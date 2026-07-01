# Criterion ships a pkg-config file whose Requires.private (boxfort) is unmet in
# nixpkgs; link the shared lib directly instead — its NEEDED deps are baked in.
find_path(CRITERION_INCLUDE_DIR criterion/criterion.h)
find_library(CRITERION_LIBRARY criterion)
if(NOT CRITERION_INCLUDE_DIR OR NOT CRITERION_LIBRARY)
  message(FATAL_ERROR "Criterion not found (need pkgs.criterion in the dev shell)")
endif()

add_executable(test_ecs test_ecs.c)
target_include_directories(test_ecs PRIVATE ${CRITERION_INCLUDE_DIR})
target_link_libraries(test_ecs PRIVATE kiln_ecs ${CRITERION_LIBRARY} m)
add_test(NAME ecs COMMAND test_ecs)

add_executable(test_arena test_arena.c)
target_include_directories(test_arena PRIVATE ${CRITERION_INCLUDE_DIR})
target_link_libraries(test_arena PRIVATE kiln_core ${CRITERION_LIBRARY} m)
add_test(NAME arena COMMAND test_arena)

add_executable(test_math test_math.c)
target_include_directories(test_math PRIVATE ${CRITERION_INCLUDE_DIR})
target_link_libraries(test_math PRIVATE kiln_core ${CRITERION_LIBRARY} m)
add_test(NAME math COMMAND test_math)

add_executable(test_physics test_physics.c)
target_include_directories(test_physics PRIVATE ${CRITERION_INCLUDE_DIR})
target_link_libraries(test_physics PRIVATE kiln_physics kiln_core ${CRITERION_LIBRARY} m)
add_test(NAME physics COMMAND test_physics)

add_executable(test_core_geom test_core_geom.c)
target_include_directories(test_core_geom PRIVATE ${CRITERION_INCLUDE_DIR})
target_link_libraries(test_core_geom PRIVATE kiln_core ${CRITERION_LIBRARY} m)
add_test(NAME core_geom COMMAND test_core_geom)

add_executable(test_assets test_assets.c)
target_include_directories(test_assets PRIVATE ${CRITERION_INCLUDE_DIR})
target_link_libraries(test_assets PRIVATE kiln_assets kiln_core ${CRITERION_LIBRARY} m)
add_test(NAME assets COMMAND test_assets)

add_executable(test_voxel test_voxel.c)
target_include_directories(test_voxel PRIVATE ${CRITERION_INCLUDE_DIR})
target_link_libraries(test_voxel PRIVATE kiln_voxel kiln_core ${CRITERION_LIBRARY} m)
add_test(NAME voxel COMMAND test_voxel)
