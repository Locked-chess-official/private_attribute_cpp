cmake_minimum_required(VERSION 3.15...3.31)

project(private_attribute_cpp LANGUAGES CXX)

# Locate the running interpreter and its development headers/module support.
find_package(
  Python
  COMPONENTS Interpreter Development.Module
  REQUIRED)

# Build the `private_attribute` extension module with a proper SOABI suffix
# (private_attribute.cpython-3XX[-t]-<plat>.so), importable from site-packages.
Python_add_library(private_attribute MODULE WITH_SOABI private_attribute.cpp)

# picosha2.h is a header-only dependency living next to the .cpp.
target_include_directories(private_attribute PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# C++17, matching the original setup.py flags.
if(MSVC)
  target_compile_options(private_attribute PRIVATE /std:c++17 /GR-)
else()
  target_compile_options(private_attribute PRIVATE -std=c++17 -fno-exceptions -fno-rtti)
  # Strip debug info unless the caller explicitly opts in to keeping it.
  # (This is the conditional that a static pyproject.toml cannot express,
  #  hence it lives here in CMake instead of setup.py.)
  if(NOT "$ENV{PRIVATE_ATTR_KEEP_DEBUG}" STREQUAL "1")
    target_compile_options(private_attribute PRIVATE -g0)
  endif()
endif()

# Install both the compiled module and its PEP 561 type stub at the
# site-packages root, side by side.
install(TARGETS private_attribute LIBRARY DESTINATION .)
install(FILES private_attribute.pyi DESTINATION .)
