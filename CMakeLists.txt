cmake_minimum_required(VERSION 3.11)
project(markit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(markit
  src/main.cpp
  src/scroller.cpp
  src/markdown.cpp
  src/config.cpp
)
target_link_libraries(markit
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE md4c
  PRIVATE yaml-cpp
)
target_include_directories(markit PRIVATE ${md4c_SOURCE_DIR}/src)

option(MARKIT_BUILD_TESTS "Build markit tests" ON)
if (MARKIT_BUILD_TESTS)
  enable_testing()

  if (NOT BUILD_SHARED_LIBS)
    include(${ftxui_SOURCE_DIR}/cmake/ftxui_find_google_test.cmake)

    add_executable(markit-tests
      src/scroller_test.cpp
      src/scroller.cpp
      src/markdown_test.cpp
      src/markdown.cpp
      src/config_test.cpp
      src/config.cpp
    )
    target_include_directories(markit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(markit-tests
      PRIVATE ftxui::component
      PRIVATE ftxui::dom
      PRIVATE ftxui::screen
      PRIVATE GTest::gtest_main
      PRIVATE md4c
      PRIVATE yaml-cpp
    )
    target_include_directories(markit-tests PRIVATE ${md4c_SOURCE_DIR}/src)
    add_test(NAME markit-tests COMMAND markit-tests)
  else()
    message(STATUS "markit: tests require BUILD_SHARED_LIBS=OFF; skipped")
  endif()
endif()
