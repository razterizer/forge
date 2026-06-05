add_executable(forge_cli_tests
    cli_test.cpp
)
target_link_libraries(forge_cli_tests PRIVATE forge::core)

add_test(NAME forge.cli COMMAND forge_cli_tests)

