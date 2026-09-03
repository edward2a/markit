cmake_minimum_required(VERSION 3.11)
project(markit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG        v7.0.3
)
FetchContent_MakeAvailable(ftxui)

add_executable(markit
  src/main.cpp
  src/scroller.cpp
)
target_link_libraries(markit
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
)

option(MARKIT_BUILD_TESTS "Build markit tests" ON)
if (MARKIT_BUILD_TESTS)
  enable_testing()

  if (NOT BUILD_SHARED_LIBS)
    include(${ftxui_SOURCE_DIR}/cmake/ftxui_find_google_test.cmake)

    add_executable(markit-tests
      src/scroller_test.cpp
      src/scroller.cpp
    )
    target_include_directories(markit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(markit-tests
      PRIVATE ftxui::component
      PRIVATE ftxui::dom
      PRIVATE ftxui::screen
      PRIVATE GTest::gtest_main
    )
    add_test(NAME markit-tests COMMAND markit-tests)
  else()
    message(STATUS "markit: tests require BUILD_SHARED_LIBS=OFF; skipped")
  endif()
endif()
