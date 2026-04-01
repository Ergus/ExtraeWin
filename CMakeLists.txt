
cmake_minimum_required(VERSION 3.10)

project(ExtraeWin LANGUAGES CXX)

# Set C++ 17 standard with no extensions
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED On)

# Variadic macros with no arguments are an extension, but available in
# gcc, clan and msvc
# set(CMAKE_CXX_EXTENSIONS Off)

# Compile main with TB (for policies) and WALL
if (CMAKE_CXX_COMPILER_ID MATCHES "MSVC") # warning level 4
    add_compile_options(/W4)
else() # additional warnings
    find_package(TBB QUIET)               # optional: required only for parallel policies on older GCC
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(WITH_COVERAGE "Enable gcov coverage instrumentation" OFF)
    if(WITH_COVERAGE)
        add_compile_options(--coverage)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    endif()
endif()

add_executable(tests_profiler.x EXCLUDE_FROM_ALL tests/tests_profiler.cpp)
target_compile_definitions(tests_profiler.x PRIVATE PROFILER_ENABLED=2)
target_include_directories(tests_profiler.x PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC" AND TBB_FOUND)
    target_link_libraries(tests_profiler.x TBB::tbb)
endif()


add_executable(Parser.x Parser.cpp)

add_executable(test_parser.x EXCLUDE_FROM_ALL tests/test_parser.cpp)

enable_testing()

# Build test binaries on demand — runs as the first CTest step via a fixture,
# so a plain "cmake --build ." never compiles the test executables.
add_test(NAME build_test_binaries
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --config $<CONFIG>
            --target tests_profiler.x test_parser.x)
set_tests_properties(build_test_binaries PROPERTIES
    FIXTURES_SETUP  test_binaries_built
    TIMEOUT         300)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tests/tests_profiler.cpp" tests_profiler_cpp)
string(REGEX MATCHALL "\nDEFINE_TEST\\([^)]+\\)" matches "${tests_profiler_cpp}")
foreach(match ${matches})
    string(REGEX MATCH "\nDEFINE_TEST\\(([^)]+)\\)" _ "${match}")
    add_test(NAME "${CMAKE_MATCH_1}" COMMAND $<TARGET_FILE:tests_profiler.x> "${CMAKE_MATCH_1}")
    set_tests_properties("${CMAKE_MATCH_1}" PROPERTIES FIXTURES_REQUIRED test_binaries_built)
endforeach()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parser.cpp" test_parser_cpp)
string(REGEX MATCHALL "\nDEFINE_TEST\\([^)]+\\)" matches "${test_parser_cpp}")
foreach(match ${matches})
    string(REGEX MATCH "\nDEFINE_TEST\\(([^)]+)\\)" _ "${match}")
    add_test(NAME "${CMAKE_MATCH_1}"
        COMMAND $<TARGET_FILE:test_parser.x> "${CMAKE_MATCH_1}" $<TARGET_FILE:Parser.x>)
    set_tests_properties("${CMAKE_MATCH_1}" PROPERTIES FIXTURES_REQUIRED test_binaries_built)
endforeach()
