cmake_minimum_required(VERSION 4.5.0)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PARSLEY_VERSION)
string(STRIP "${PARSLEY_VERSION}" PARSLEY_VERSION)

project(parsley VERSION "${PARSLEY_VERSION}" LANGUAGES C CXX)

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
      src/version.cppm
      src/diagnostic.cppm
      src/lexer.cppm
      src/ast.cppm
      src/parser.cppm
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(parsley_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PARSLEY_BUILD_TESTS)
  enable_testing()
  add_executable(parsley_tests tests/test_main.cpp)
  target_compile_features(parsley_tests PRIVATE cxx_std_26)
  target_link_libraries(parsley_tests PRIVATE parsley_core)
  target_compile_definitions(parsley_tests PRIVATE
    PARSLEY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    PARSLEY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    PARSLEY_C_COMPILER="${CMAKE_C_COMPILER}"
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(parsley_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME parsley_unit_tests COMMAND parsley_tests)
endif()

install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
  "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
  DESTINATION share/doc/parsley
)

set(CPACK_PACKAGE_NAME "parsley")
set(CPACK_PACKAGE_VENDOR "Parsley Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Parsley — Haskell-style DSL for declarative grammar definitions and ISO C17 parser generation")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_GENERATOR "ZIP;TGZ")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
