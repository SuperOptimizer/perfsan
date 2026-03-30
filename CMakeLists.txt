cmake_minimum_required(VERSION 3.20)
project(PerfSanitizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Find LLVM and Clang ----
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

# ---- Include paths ----
include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

# ---- Shared plugin library (loadable by clang and opt) ----
# IMPORTANT: Do NOT link LLVM/Clang libs — the host clang already has them.
# Linking them causes duplicate symbol registration and crashes.
add_llvm_library(PerfSanitizer MODULE
  lib/PerfHint.cpp
  lib/AST/PerfASTVisitor.cpp
  lib/AST/PerfASTExtraChecks.cpp
  lib/IR/PerfIRPass.cpp
  lib/Plugin.cpp

  PLUGIN_TOOL clang
)

target_include_directories(PerfSanitizer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# No target_link_libraries for LLVM/Clang — symbols resolved at load time
# from the host clang/opt binary. On Linux, undefined symbols in MODULE
# libraries are resolved from the loading executable.
