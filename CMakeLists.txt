cmake_minimum_required(VERSION 3.20)
project(tinydb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(TINYDB_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(TINYDB_FETCH_GTEST "Download pinned GoogleTest if no installed package is found" OFF)
set(TINYDB_GTEST_SOURCE_DIR "" CACHE PATH "Optional local GoogleTest source directory")

find_package(Threads REQUIRED)
add_library(tinydb
  src/storage/page.cpp
  src/storage/disk_manager.cpp
  src/storage/slotted_page.cpp
  src/storage/heap_file.cpp
  src/storage/record.cpp)
target_include_directories(tinydb PUBLIC include)
target_link_libraries(tinydb PUBLIC Threads::Threads)
target_compile_options(tinydb PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
if(TINYDB_SANITIZERS)
  target_compile_options(tinydb PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(tinydb PUBLIC -fsanitize=address,undefined)
endif()

add_executable(storage_demo examples/storage_demo.cpp)
target_link_libraries(storage_demo PRIVATE tinydb)

include(CTest)
if(BUILD_TESTING)
  if(TINYDB_GTEST_SOURCE_DIR)
    add_subdirectory("${TINYDB_GTEST_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
  else()
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
      if(NOT TINYDB_FETCH_GTEST)
        message(FATAL_ERROR "Install libgtest-dev, set TINYDB_GTEST_SOURCE_DIR, or enable TINYDB_FETCH_GTEST")
      endif()
      include(FetchContent)
      # Fixed upstream release commit; only used when explicitly requested.
      FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571)
      FetchContent_MakeAvailable(googletest)
    endif()
  endif()
  add_executable(storage_tests tests/storage_tests.cpp)
  target_link_libraries(storage_tests PRIVATE tinydb GTest::gtest_main)
  target_compile_options(storage_tests PRIVATE -Wall -Wextra -Wpedantic)
  include(GoogleTest)
  gtest_discover_tests(storage_tests DISCOVERY_TIMEOUT 30)
endif()
