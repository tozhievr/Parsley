cmake_minimum_required(VERSION 3.20)

# Determine root directory relative to CMAKE_CURRENT_LIST_DIR/..
get_filename_component(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(VERSION_FILE "${ROOT_DIR}/VERSION")
set(VERSION_CPPM_FILE "${ROOT_DIR}/src/version.cppm")

if(NOT EXISTS "${VERSION_FILE}")
  message(FATAL_ERROR "VERSION file not found at ${VERSION_FILE}")
endif()

# Read VERSION. Strip whitespace.
file(READ "${VERSION_FILE}" OLD_VERSION)
string(STRIP "${OLD_VERSION}" OLD_VERSION)

# Parse MAJOR.MINOR.PATCH.
if(NOT OLD_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  message(FATAL_ERROR "Invalid version format in ${VERSION_FILE}: '${OLD_VERSION}'")
endif()

set(MAJOR "${CMAKE_MATCH_1}")
set(MINOR "${CMAKE_MATCH_2}")
set(PATCH "${CMAKE_MATCH_3}")

# If DEFINED SET_VERSION, validate X.Y.Z format and use it.
if(DEFINED SET_VERSION AND NOT SET_VERSION STREQUAL "")
  string(STRIP "${SET_VERSION}" SET_VERSION)
  if(NOT SET_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "Invalid SET_VERSION format: '${SET_VERSION}'. Expected X.Y.Z format (e.g. 1.2.3)")
  endif()
  set(NEW_MAJOR "${CMAKE_MATCH_1}")
  set(NEW_MINOR "${CMAKE_MATCH_2}")
  set(NEW_PATCH "${CMAKE_MATCH_3}")
  set(NEW_VERSION "${SET_VERSION}")
else()
  # Else check BUMP (default patch): major -> (MAJOR+1).0.0, minor -> MAJOR.(MINOR+1).0, patch -> MAJOR.MINOR.(PATCH+1)
  if(NOT DEFINED BUMP OR BUMP STREQUAL "")
    set(BUMP "patch")
  endif()
  string(TOLOWER "${BUMP}" BUMP)

  if(BUMP STREQUAL "major")
    math(EXPR NEW_MAJOR "${MAJOR} + 1")
    set(NEW_MINOR 0)
    set(NEW_PATCH 0)
  elseif(BUMP STREQUAL "minor")
    set(NEW_MAJOR "${MAJOR}")
    math(EXPR NEW_MINOR "${MINOR} + 1")
    set(NEW_PATCH 0)
  elseif(BUMP STREQUAL "patch")
    set(NEW_MAJOR "${MAJOR}")
    set(NEW_MINOR "${MINOR}")
    math(EXPR NEW_PATCH "${PATCH} + 1")
  else()
    message(FATAL_ERROR "Unknown BUMP option: '${BUMP}'. Expected 'patch', 'minor', or 'major'.")
  endif()
  set(NEW_VERSION "${NEW_MAJOR}.${NEW_MINOR}.${NEW_PATCH}")
endif()

# Write new VERSION file (with trailing \n)
file(WRITE "${VERSION_FILE}" "${NEW_VERSION}\n")

# Regenerate src/version.cppm
set(CPPM_CONTENT
"module;
#include <string_view>

export module parsley.version;

export namespace parsley {

inline constexpr int VERSION_MAJOR = ${NEW_MAJOR};
inline constexpr int VERSION_MINOR = ${NEW_MINOR};
inline constexpr int VERSION_PATCH = ${NEW_PATCH};
inline constexpr std::string_view VERSION_STRING = \"${NEW_VERSION}\";

} // namespace parsley
")

file(WRITE "${VERSION_CPPM_FILE}" "${CPPM_CONTENT}")

# Print Updated Parsley version: <old> -> <new> via message(STATUS ...)
message(STATUS "Updated Parsley version: ${OLD_VERSION} -> ${NEW_VERSION}")
