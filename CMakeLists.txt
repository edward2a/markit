cmake_minimum_required(VERSION 3.11)
project(markit LANGUAGES CXX)

function(markit_changelog_version CONTENT OUT_VAR)
  string(REGEX MATCH "## ([0-9]+\\.[0-9]+\\.[0-9]+)" _match "${CONTENT}")
  if (_match)
    set(${OUT_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  else()
    set(${OUT_VAR} "" PARENT_SCOPE)
  endif()
endfunction()

# Version output is generated during configuration so the binary does not
# need Git at runtime. Release tags are made on master; a newer changelog
# section on another branch is therefore an unreleased version.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md" _markit_changelog)
markit_changelog_version("${_markit_changelog}" _markit_current_version)

set(_markit_version_string "devel-unknown")
find_package(Git QUIET)
if (GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --is-inside-work-tree
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _markit_git_worktree
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _markit_git_result
  )
  if (_markit_git_result EQUAL 0 AND _markit_git_worktree STREQUAL "true")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE _markit_commit_id
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _markit_commit_result
    )
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" symbolic-ref --quiet --short HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE _markit_branch
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _markit_branch_result
    )
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
              --match "[0-9]*.[0-9]*.[0-9]*" HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE _markit_exact_tag
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _markit_tag_result
    )

    if (_markit_commit_result EQUAL 0)
      set(_markit_channel "devel")
      if (_markit_branch STREQUAL "master" OR _markit_tag_result EQUAL 0)
        set(_markit_channel "master")
      endif()

      # Prefer the local master ref, then origin/master, and finally the
      # latest reachable release tag for shallow or detached checkouts.
      set(_markit_master_changelog "")
      foreach (_markit_master_ref master origin/master)
        execute_process(
          COMMAND "${GIT_EXECUTABLE}" show
                  "${_markit_master_ref}:CHANGELOG.md"
          WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
          OUTPUT_VARIABLE _markit_master_candidate
          ERROR_QUIET
          RESULT_VARIABLE _markit_master_result
        )
        if (_markit_master_result EQUAL 0)
          set(_markit_master_changelog "${_markit_master_candidate}")
          break()
        endif()
      endforeach()
      if (_markit_master_changelog STREQUAL "")
        execute_process(
          COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
                  --match "[0-9]*.[0-9]*.[0-9]*" HEAD
          WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
          OUTPUT_VARIABLE _markit_latest_tag
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET
          RESULT_VARIABLE _markit_latest_tag_result
        )
        if (_markit_latest_tag_result EQUAL 0)
          execute_process(
            COMMAND "${GIT_EXECUTABLE}" show
                    "${_markit_latest_tag}:CHANGELOG.md"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _markit_master_changelog
            ERROR_QUIET
          )
        endif()
      endif()
      markit_changelog_version(
        "${_markit_master_changelog}" _markit_master_version
      )

      if (_markit_current_version STREQUAL "")
        set(_markit_current_version "unknown")
      endif()
      if (_markit_channel STREQUAL "master")
        set(_markit_version_string
            "${_markit_current_version}-master-${_markit_commit_id}")
      elseif (NOT _markit_master_version STREQUAL "" AND
              NOT _markit_current_version STREQUAL _markit_master_version)
        set(_markit_version_string
            "${_markit_current_version}-devel-${_markit_commit_id}")
      else()
        set(_markit_version_string "devel-${_markit_commit_id}")
      endif()
    endif()
  endif()
endif()
message(STATUS "markit version: ${_markit_version_string}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Enforce the strict ISO standard (-std=c++17) instead of the GNU dialect
# (-std=gnu++17), so the build does not depend on the toolchain default.
set(CMAKE_CXX_EXTENSIONS OFF)

option(MARKIT_BUILD_TESTS "Build markit tests" ON)
option(MARKIT_BUILD_DEPENDENCY_TOOLS
       "Build utility targets from vendored dependencies" ON)
if (NOT MARKIT_BUILD_DEPENDENCY_TOOLS)
  set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL
      "Whether to compile the md2html executable" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL
      "Enable yaml-cpp contrib in library" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL
      "Enable yaml-cpp parse tools" FORCE)
endif()

# yaml-cpp 0.8.0 declares cmake_minimum_required(3.5); newer CMake rejects
# configuring a subproject below its minimum supported policy version unless
# this minimum is relaxed.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Build FTXUI and md4c as static libraries. Without this pinning, either
# dependency can flip BUILD_SHARED_LIBS, silently changing the linkage of
# the whole project (and the markit binary shrank from ~1.7MB to ~280KB when
# FTXUI was rebuilt as shared). Keep every consumer static.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG        v7.0.3
)
FetchContent_MakeAvailable(ftxui)

FetchContent_Declare(md4c
  GIT_REPOSITORY https://github.com/mity/md4c
  GIT_TAG        release-0.5.2
)
# md4c's CMakeLists sets BUILD_SHARED_LIBS=ON on Linux; the FORCE above keeps
# it static regardless.
FetchContent_MakeAvailable(md4c)

FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp
  GIT_TAG        0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# RE2 (2025-11-05) builds against Abseil unconditionally, so both come via
# FetchContent. ABSL_PROPAGATE_CXX_STD keeps Abseil on our C++ standard; the
# BUILD_SHARED_LIBS=OFF pin above keeps both static.
set(ABSL_PROPAGATE_CXX_STD ON)
FetchContent_Declare(abseil
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG        20250814.1
)
FetchContent_MakeAvailable(abseil)

FetchContent_Declare(re2
  GIT_REPOSITORY https://github.com/google/re2.git
  GIT_TAG        2025-11-05
)
# RE2's install(EXPORT) rules assume an installed Abseil; markit never
# installs, so skip them (avoids export-set errors with vendored Abseil).
set(RE2_INSTALL OFF)
FetchContent_MakeAvailable(re2)

# Shared core: compiled once and linked by both the app and the tests.
add_library(markit_core STATIC
  src/scroller.cpp
  src/markdown.cpp
  src/chrome.cpp
  src/anchor.cpp
  src/config.cpp
  src/search.cpp
)
target_include_directories(markit_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE ${md4c_SOURCE_DIR}/src
)
target_link_libraries(markit_core
  PUBLIC ftxui::component
  PUBLIC ftxui::dom
  PUBLIC ftxui::screen
  PUBLIC md4c
  PUBLIC yaml-cpp
  PRIVATE re2::re2
)

add_executable(markit
  src/main.cpp
)
target_compile_definitions(markit
  PRIVATE "MARKIT_VERSION=\"${_markit_version_string}\""
)
target_link_libraries(markit
  PRIVATE markit_core
)

if (MARKIT_BUILD_TESTS)
  enable_testing()
  add_test(NAME markit-version COMMAND markit --version)
  set_tests_properties(markit-version PROPERTIES
    PASS_REGULAR_EXPRESSION "markit ${_markit_version_string}"
  )

  if (NOT BUILD_SHARED_LIBS)
    include(${ftxui_SOURCE_DIR}/cmake/ftxui_find_google_test.cmake)

    add_executable(markit-tests
      src/scroller_test.cpp
      src/markdown_test.cpp
      src/chrome_test.cpp
      src/anchor_test.cpp
      src/config_test.cpp
      src/search_test.cpp
    )
    target_link_libraries(markit-tests
      PRIVATE markit_core
      PRIVATE GTest::gtest_main
    )
    add_test(NAME markit-tests COMMAND markit-tests)
  else()
    message(STATUS "markit: tests require BUILD_SHARED_LIBS=OFF; skipped")
  endif()
endif()

option(MARKIT_BUILD_BENCHMARKS "Build opt-in markit benchmarks" OFF)
if (MARKIT_BUILD_BENCHMARKS)
  add_executable(markit-bench
    benchmarks/bench_main.cpp
  )
  target_link_libraries(markit-bench
    PRIVATE markit_core
  )
endif()
