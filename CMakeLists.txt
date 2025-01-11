cmake_minimum_required(VERSION 3.10)
project(LuauProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Luau subdirectory
add_subdirectory(luau)

# Add executable
add_executable(main main.cpp)

# Link against Luau libraries
target_link_libraries(main PRIVATE 
    Luau.Ast
    Luau.Compiler
    Luau.VM
    Luau.Analysis
    Luau.CodeGen
    Luau.CLI.lib
    isocline
)

# Include directories
target_include_directories(main PRIVATE
    ${CMAKE_SOURCE_DIR}/luau/Common/include
    ${CMAKE_SOURCE_DIR}/luau/Ast/include
    ${CMAKE_SOURCE_DIR}/luau/Compiler/include
    ${CMAKE_SOURCE_DIR}/luau/VM/include
    ${CMAKE_SOURCE_DIR}/luau/Analysis/include
    ${CMAKE_SOURCE_DIR}/luau/CodeGen/include
    ${CMAKE_SOURCE_DIR}/luau/CLI/include
    ${CMAKE_SOURCE_DIR}/luau/CLI/src
    ${CMAKE_SOURCE_DIR}/luau/extern/isocline/include
)
