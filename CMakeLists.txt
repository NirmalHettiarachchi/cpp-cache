cmake_minimum_required(VERSION 3.16)

project(cpp-cache VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CPP_CACHE_BUILD_EXAMPLES "Build cpp-cache examples" ON)
option(CPP_CACHE_BUILD_TESTS "Build cpp-cache tests" ON)
option(CPP_CACHE_BUILD_BENCHMARKS "Build cpp-cache benchmark" ON)
option(CPP_CACHE_ENABLE_TSAN "Build targets with ThreadSanitizer" OFF)

find_package(Threads REQUIRED)

add_library(cpp_cache INTERFACE)
target_include_directories(
  cpp_cache INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                      $<INSTALL_INTERFACE:include>)
target_compile_features(cpp_cache INTERFACE cxx_std_17)

function(cpp_cache_configure_target target)
  set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
                                             CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /O2)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -O2)
    if(CPP_CACHE_ENABLE_TSAN)
      target_compile_options(${target} PRIVATE -fsanitize=thread -g -O1)
      target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
  endif()
endfunction()

if(CPP_CACHE_BUILD_EXAMPLES)
  set(CPP_CACHE_EXAMPLES examples_lru basic_lru examples_lfu lfu_example examples_ttl
                         ttl_session examples_mt multithreaded examples_memoize memoize_example
                         examples_loader loader_example examples_callback eviction_callback)

  list(LENGTH CPP_CACHE_EXAMPLES CPP_CACHE_EXAMPLE_COUNT)
  math(EXPR CPP_CACHE_EXAMPLE_LAST "${CPP_CACHE_EXAMPLE_COUNT} - 1")
  foreach(index RANGE 0 ${CPP_CACHE_EXAMPLE_LAST} 2)
    list(GET CPP_CACHE_EXAMPLES ${index} target)
    math(EXPR source_index "${index} + 1")
    list(GET CPP_CACHE_EXAMPLES ${source_index} source)
    add_executable(${target} examples/${source}.cpp)
    target_link_libraries(${target} PRIVATE cpp_cache Threads::Threads)
    cpp_cache_configure_target(${target})
  endforeach()
endif()

if(CPP_CACHE_BUILD_BENCHMARKS)
  add_executable(benchmarks benchmarks/bench.cpp)
  target_link_libraries(benchmarks PRIVATE cpp_cache Threads::Threads)
  cpp_cache_configure_target(benchmarks)
endif()

if(CPP_CACHE_BUILD_TESTS)
  include(CTest)
  include(FetchContent)

  FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(doctest)

  add_executable(tests tests/test_cache.cpp)
  target_link_libraries(tests PRIVATE cpp_cache doctest::doctest Threads::Threads)
  cpp_cache_configure_target(tests)
  add_test(NAME tests COMMAND tests)
endif()
