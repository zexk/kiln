add_executable(test_ecs test_ecs.c)
target_link_libraries(test_ecs PRIVATE kiln_ecs)
# Tests assert their way to a pass/fail; keep assert() live even in a Release build.
target_compile_options(test_ecs PRIVATE -UNDEBUG)
add_test(NAME ecs COMMAND test_ecs)
