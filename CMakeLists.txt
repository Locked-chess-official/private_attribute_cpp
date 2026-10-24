cmake_minimum_required(VERSION 3.15...3.31)

project(private_attribute_cpp LANGUAGES CXX)

# Locate the running interpreter and its development headers/module support.
find_package(
  Python
  COMPONENTS Interpreter Development.Module
  REQUIRED)

# Build the `private_attribute` extension module with a proper SOABI suffix
# (private_attribute.cpython-3XX[-t]-<plat>.so), importable from site-packages.
# Detect whether the running Python was built with --enable-shared.
execute_process(
  COMMAND "${Python_EXECUTABLE}" -c
          "import sysconfig; print(sysconfig.get_config_var('Py_ENABLE_SHARED') or 0)"
  OUTPUT_VARIABLE PYTHON_ENABLE_SHARED
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE PYTHON_CONFIG_RESULT
)

if(NOT PYTHON_CONFIG_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to query Python build configuration")
endif()

if(WIN32)
  # Windows: keep Python::Module to obtain the required import library.
  Python_add_library(
    private_attribute MODULE WITH_SOABI
    private_attribute.cpp
  )
elseif(PYTHON_ENABLE_SHARED STREQUAL "1")
  # Keep the standard CMake Python extension-module behavior.
  Python_add_library(
    private_attribute MODULE WITH_SOABI
    private_attribute.cpp
  )
else()
  # Match the usual Unix setuptools.Extension behavior:
  # do not link the static libpython into the extension module.
  add_library(
    private_attribute MODULE
    private_attribute.cpp
  )

  if(APPLE)
    target_link_options(
      private_attribute PRIVATE
      "-undefined" "dynamic_lookup"
    )
  endif()

  target_include_directories(
    private_attribute PRIVATE
    ${Python_INCLUDE_DIRS}
  )

  set_target_properties(
    private_attribute PROPERTIES
    PREFIX ""
    SUFFIX ".${Python_SOABI}${CMAKE_SHARED_MODULE_SUFFIX}"
  )

  message(STATUS
    "Python was built without --enable-shared; "
    "building the extension without linking libpython"
  )
endif()

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
