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

add_executable(forge_run_unit_tests
    run_test.cpp
)
target_link_libraries(forge_run_unit_tests PRIVATE forge::core)

add_test(NAME forge.run.unit COMMAND forge_run_unit_tests)
set_tests_properties(forge.run.unit PROPERTIES LABELS unit)

add_executable(forge_test_unit_tests
    test_test.cpp
)
target_link_libraries(forge_test_unit_tests PRIVATE forge::core)

add_test(NAME forge.test.unit COMMAND forge_test_unit_tests)
set_tests_properties(forge.test.unit PROPERTIES LABELS unit)

add_executable(forge_release_unit_tests
    release_test.cpp
)
target_link_libraries(forge_release_unit_tests PRIVATE forge::core)

add_test(NAME forge.release.unit COMMAND forge_release_unit_tests)
set_tests_properties(forge.release.unit PROPERTIES LABELS unit)

add_executable(forge_box_unit_tests
    box_test.cpp
)
target_link_libraries(forge_box_unit_tests PRIVATE forge::core)

add_test(NAME forge.box.unit COMMAND forge_box_unit_tests)
set_tests_properties(forge.box.unit PROPERTIES LABELS unit)

add_executable(forge_workspace_unit_tests
    workspace_test.cpp
)
target_link_libraries(forge_workspace_unit_tests PRIVATE forge::core)

add_test(NAME forge.workspace.unit COMMAND forge_workspace_unit_tests)
set_tests_properties(forge.workspace.unit PROPERTIES LABELS unit)
