cmake_minimum_required(VERSION 4.5.0)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PARSLEY_VERSION)
string(STRIP "${PARSLEY_VERSION}" PARSLEY_VERSION)

project(parsley VERSION "${PARSLEY_VERSION}" LANGUAGES C CXX)
include(cmake/EmbedNextc.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
configure_file(src/version.cppm.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.cppm" @ONLY)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 23)
  message(FATAL_ERROR "Parsley requires Clang 23 or newer with C++26 module support; generated parsers require only an ISO C23 compiler.")
endif()
option(PARSLEY_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(PARSLEY_SANITIZERS)
  if(WIN32)
    message(FATAL_ERROR "PARSLEY_SANITIZERS currently requires a Linux/macOS Clang toolchain")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

option(PARSLEY_BUILD_TESTS "Build Parsley unit and integration tests" ON)

add_library(parsley_core STATIC)
target_compile_features(parsley_core PUBLIC cxx_std_26)
target_sources(parsley_core
  PUBLIC
    FILE_SET CXX_MODULES FILES
      "${CMAKE_CURRENT_BINARY_DIR}/generated/version.cppm"
      src/diagnostic.cppm
      src/lexer.cppm
      src/ast.cppm
      src/parser.cppm
      src/stdlib.cppm
      src/resolve.cppm
      src/typecheck.cppm
      src/ir.cppm
      src/desugar.cppm
      src/analysis.cppm
      src/regex.cppm
      src/eval.cppm
      src/psl.cppm
      src/c23_text.cppm
      src/c23_runtime.cppm
      src/optimizer_runtime.cppm
      src/optimizer.cppm
      "${CMAKE_CURRENT_BINARY_DIR}/generated/nextc_bundle.cppm"
      src/native_driver.cppm
      src/native_backend.cppm
      src/codegen.cppm
      src/tooling.cppm
      src/compiler.cppm
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(parsley_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(parsley src/main.cpp)
target_compile_features(parsley PRIVATE cxx_std_26)
target_link_libraries(parsley PRIVATE parsley_core)
target_compile_definitions(parsley PRIVATE PARSLEY_DEFAULT_CC="${CMAKE_C_COMPILER}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(parsley PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PARSLEY_BUILD_TESTS)
  enable_testing()
  add_executable(parsley_tests tests/test_main.cpp)
  target_compile_features(parsley_tests PRIVATE cxx_std_26)
  target_link_libraries(parsley_tests PRIVATE parsley_core)
  add_dependencies(parsley_tests parsley)
  target_compile_definitions(parsley_tests PRIVATE
    PARSLEY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    PARSLEY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    PARSLEY_C_COMPILER="${CMAKE_C_COMPILER}"
    PARSLEY_CLI_EXE="$<TARGET_FILE:parsley>"
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(parsley_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME parsley_unit_tests COMMAND parsley_tests)
  add_test(NAME parsley_cli_version COMMAND parsley --version)
  add_test(NAME parsley_cli_check_calc COMMAND parsley check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Calc.parsley")
  add_test(NAME parsley_cli_test_calc COMMAND parsley test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Calc.parsley" --fuzz 50)
  add_test(NAME parsley_cli_explain_e0214 COMMAND parsley explain E0214)
  add_test(NAME parsley_cli_pkg_list COMMAND parsley pkg list)
  add_test(NAME parsley_cli_check_python COMMAND parsley check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Python.parsley")
  add_test(NAME parsley_cli_test_python COMMAND parsley test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Python.parsley" --fuzz 30)
  add_test(NAME parsley_cli_check_swift COMMAND parsley check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Swift.parsley")
  add_test(NAME parsley_cli_test_swift COMMAND parsley test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Swift.parsley" --fuzz 30)
  add_test(NAME parsley_cli_check_kotlin COMMAND parsley check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Kotlin.parsley")
  add_test(NAME parsley_cli_test_kotlin COMMAND parsley test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Kotlin.parsley" --fuzz 30)
  add_test(NAME parsley_cli_check_haskell COMMAND parsley check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Haskell.parsley")
  add_test(NAME parsley_cli_test_haskell COMMAND parsley test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Haskell.parsley" --fuzz 30)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME parsley_audit_regressions COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit_regression.py" --cli "$<TARGET_FILE:parsley>" --cc "${CMAKE_C_COMPILER}")
    set_tests_properties(parsley_audit_regressions PROPERTIES TIMEOUT 180)
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME parsley_optimizer_regressions COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/optimizer_regression.py" --cli "$<TARGET_FILE:parsley>" --cc "${CMAKE_C_COMPILER}")
    set_tests_properties(parsley_optimizer_regressions PROPERTIES TIMEOUT 180)
  endif()
  add_test(NAME parsley_cli_check_optimizer COMMAND parsley check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Optimizer.parsley")
  add_test(NAME parsley_cli_test_optimizer COMMAND parsley test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Optimizer.parsley" --fuzz 30)
  if(WIN32 AND Python3_Interpreter_FOUND)
    add_test(NAME parsley_native_backend_regressions COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_backend_regression.py" --cli "$<TARGET_FILE:parsley>" --cc "${CMAKE_C_COMPILER}")
    set_tests_properties(parsley_native_backend_regressions PROPERTIES TIMEOUT 240)
  endif()
  find_program(PARSLEY_NODE_EXECUTABLE node)
  if(PARSLEY_NODE_EXECUTABLE)
    add_test(NAME parsley_extension_commands COMMAND "${PARSLEY_NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/editors/vscode/test/extension.test.cjs")
    set_tests_properties(parsley_extension_commands PROPERTIES TIMEOUT 30)
  endif()
  add_test(NAME parsley_cli_check_mini COMMAND parsley check "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Mini.parsley")
  add_test(NAME parsley_cli_test_mini COMMAND parsley test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/Mini.parsley" --fuzz 30)
  get_property(PARSLEY_ALL_TESTS DIRECTORY PROPERTY TESTS)
  foreach(PARSLEY_TEST IN LISTS PARSLEY_ALL_TESTS)
    if(NOT PARSLEY_TEST MATCHES "^parsley_(audit|optimizer|native_backend)_regressions$")
      set_tests_properties("${PARSLEY_TEST}" PROPERTIES TIMEOUT 120)
    endif()
  endforeach()
endif()

install(TARGETS parsley
  RUNTIME DESTINATION bin
)

install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
  "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
  "${CMAKE_CURRENT_SOURCE_DIR}/AGENTS.md"
  DESTINATION share/doc/parsley
)

install(FILES docs/STATUS.md docs/REGEX.md docs/OPTIMIZER.md docs/BACKEND.md DESTINATION share/doc/parsley/docs)
if(WIN32)
  install(FILES install.ps1 update.ps1 install.cmd update.cmd DESTINATION .)
  install(FILES editors/vscode/parsley-lang.vsix DESTINATION share/parsley/editors OPTIONAL)
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/skills"
  DESTINATION share/parsley OPTIONAL
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/editors/vscode"
  DESTINATION share/parsley/editors OPTIONAL
  PATTERN "*.vsix" EXCLUDE
  PATTERN "node_modules" EXCLUDE
  PATTERN "test" EXCLUDE
  PATTERN "*.map" EXCLUDE
  PATTERN "package-lock.json" EXCLUDE
)

set(CPACK_PACKAGE_NAME "parsley")
set(CPACK_PACKAGE_VENDOR "Parsley Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Parsley — Haskell-style DSL for declarative grammar definitions and ISO C23 parser generation")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_GENERATOR "ZIP;TGZ")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
