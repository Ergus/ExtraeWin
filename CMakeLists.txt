
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
    find_package(TBB REQUIRED)            # policies requires TBB
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(main.x main.cpp)
target_compile_definitions(main.x PRIVATE PROFILER_ENABLED=2)

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC") # warning level 4
    target_link_libraries(main.x TBB::tbb)
endif()


add_executable(Parser.x Parser.cpp)

enable_testing()

add_test(NAME "main" COMMAND $<TARGET_FILE:main.x>)
