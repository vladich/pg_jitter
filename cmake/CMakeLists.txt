cmake_minimum_required(VERSION 3.16)
project(pg_jitter_build C CXX)

if(NOT DEFINED BACKEND)
    message(FATAL_ERROR "BACKEND not set. Use -DBACKEND=sljit|asmjit|mir")
endif()

# ROOT points to the pg_jitter project root (parent of cmake/)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Platform-specific shared library suffix and link options
if(APPLE)
    set(PG_MODULE_SUFFIX ".dylib")
    set(PG_MODULE_LINK_OPTIONS -bundle -undefined dynamic_lookup)
elseif(WIN32)
    set(PG_MODULE_SUFFIX ".dll")
    set(PG_MODULE_LINK_OPTIONS "")
else()
    set(PG_MODULE_SUFFIX ".so")
    set(PG_MODULE_LINK_OPTIONS "")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/${BACKEND}.cmake)
