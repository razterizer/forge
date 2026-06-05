add_executable(forge_cli_tests
    cli_test.cpp
)
target_link_libraries(forge_cli_tests PRIVATE forge::core)

add_test(NAME forge.cli COMMAND forge_cli_tests)
set_tests_properties(forge.cli PROPERTIES LABELS integration)

add_executable(forge_build_unit_tests
    build_test.cpp
)
target_link_libraries(forge_build_unit_tests PRIVATE forge::core)

add_test(NAME forge.build.unit COMMAND forge_build_unit_tests)
set_tests_properties(forge.build.unit PROPERTIES LABELS unit)
