cmake_minimum_required(VERSION 3.16)

project(cpp-cache VERSION 1.0.0 LANGUAGES CXX)

option(CPP_CACHE_BUILD_EXAMPLES "Build cpp-cache examples" ON)
option(CPP_CACHE_BUILD_TESTS "Build cpp-cache tests" ON)
option(CPP_CACHE_BUILD_BENCHMARKS "Build cpp-cache benchmark" ON)
option(CPP_CACHE_ENABLE_TSAN "Build targets with ThreadSanitizer" OFF)

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
  set(CPP_CACHE_EXAMPLES
      basic_lru
      lfu_example
      ttl_session
      multithreaded
      memoize_example
      loader_example
      eviction_callback)

  foreach(example IN LISTS CPP_CACHE_EXAMPLES)
    add_executable(${example} examples/${example}.cpp)
    target_link_libraries(${example} PRIVATE cpp_cache)
    cpp_cache_configure_target(${example})
  endforeach()
endif()

if(CPP_CACHE_BUILD_BENCHMARKS)
  add_executable(cpp_cache_bench benchmarks/bench.cpp)
  target_link_libraries(cpp_cache_bench PRIVATE cpp_cache)
  cpp_cache_configure_target(cpp_cache_bench)
endif()

if(CPP_CACHE_BUILD_TESTS)
  include(CTest)
  include(FetchContent)

  FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(doctest)

  add_executable(cpp_cache_tests tests/test_cache.cpp)
  target_link_libraries(cpp_cache_tests PRIVATE cpp_cache doctest::doctest)
  cpp_cache_configure_target(cpp_cache_tests)
  add_test(NAME cpp_cache_tests COMMAND cpp_cache_tests)
endif()

# history marker: eviction callback example target
