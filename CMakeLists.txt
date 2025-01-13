cmake_minimum_required(VERSION 3.10)
project(LuauProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Luau subdirectory to build components
add_subdirectory(luau)

# Add main executable target
add_executable(main main.cpp)

target_include_directories(main PRIVATE 
    luau/Analysis/include
    luau/Ast/include
    luau/Compiler/include
    luau/VM/include
    luau/CodeGen/include
    luau/Common/include
    luau/EqSat/include
    luau/CLI/include
    luau/Config/include
)

# Link against individual Luau libraries in the correct order
target_link_libraries(main PRIVATE 
    Luau.CLI.lib
    Luau.Analysis
    Luau.CodeGen
    Luau.Compiler
    Luau.VM
    Luau.Ast
    Luau.EqSat
    isocline
)
