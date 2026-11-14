cmake_minimum_required(VERSION 3.24)

project(machine_benchmarks
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WARNING_FLAGS "" CACHE STRING "Warning flags")
set(BENCHMARK_ARCH_FLAGS "" CACHE STRING "Architecture-specific flags")

find_package(Threads REQUIRED)

add_library(mb_common INTERFACE)
target_include_directories(mb_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(mb_common SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_link_libraries(mb_common INTERFACE Threads::Threads)
target_compile_options(mb_common INTERFACE ${WARNING_FLAGS} ${BENCHMARK_ARCH_FLAGS})

foreach(benchmark topology latency bandwidth c2c atomics)
  add_executable(${benchmark} src/${benchmark}.cpp)
  target_link_libraries(${benchmark} PRIVATE mb_common)
  set_target_properties(${benchmark} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endforeach()
