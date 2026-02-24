cmake_minimum_required(VERSION 3.16)
project(pg_jitter C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ==========================================================================
# Options & dependency paths
# ==========================================================================
#
# Usage:
#   cmake .. -DPG_CONFIG=/path/to/pg_config                      (required)
#            -DSLJIT_DIR=/path/to/sljit                           (optional)
#            -DMIR_DIR=/path/to/mir                               (optional)
#            -DASMJIT_DIR=/path/to/asmjit                         (optional)
#            -DPG_JITTER_USE_LLVM=ON                              (optional)
#            -DPG_JITTER_USE_C2MIR=ON                             (optional)
#            -DPG_JITTER_INSTALL_DIR=/custom/install/path         (optional)
#

# ---------- PostgreSQL paths via pg_config ----------
set(PG_CONFIG "pg_config" CACHE FILEPATH "Path to pg_config executable")

execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _pg_config_result)
if(NOT _pg_config_result EQUAL 0)
    message(FATAL_ERROR "pg_config failed. Set -DPG_CONFIG=/path/to/pg_config")
endif()
execute_process(COMMAND ${PG_CONFIG} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS "PG server includes: ${PG_INCLUDEDIR_SERVER}")
message(STATUS "PG pkglibdir:       ${PG_PKGLIBDIR}")

# ---------- Installation directory ----------
# By default, install into PG's pkglibdir. Override with -DPG_JITTER_INSTALL_DIR.
set(PG_JITTER_INSTALL_DIR "${PG_PKGLIBDIR}" CACHE PATH
    "Installation directory for pg_jitter shared libraries")
message(STATUS "Install directory:  ${PG_JITTER_INSTALL_DIR}")

# ---------- Dependency directories ----------
set(SLJIT_DIR "${CMAKE_SOURCE_DIR}/../sljit" CACHE PATH
    "Path to sljit source directory")
set(MIR_DIR "${CMAKE_SOURCE_DIR}/../mir" CACHE PATH
    "Path to MIR source directory")
set(ASMJIT_DIR "${CMAKE_SOURCE_DIR}/../asmjit" CACHE PATH
    "Path to AsmJIT source directory")

# Validate dependency directories
if(NOT EXISTS "${SLJIT_DIR}/sljit_src/sljitLir.c")
    message(FATAL_ERROR "sljit not found at ${SLJIT_DIR}. Set -DSLJIT_DIR=/path/to/sljit")
endif()
if(NOT EXISTS "${MIR_DIR}/mir.c")
    message(FATAL_ERROR "MIR not found at ${MIR_DIR}. Set -DMIR_DIR=/path/to/mir")
endif()
if(NOT EXISTS "${ASMJIT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "AsmJIT not found at ${ASMJIT_DIR}. Set -DASMJIT_DIR=/path/to/asmjit")
endif()

message(STATUS "sljit directory:    ${SLJIT_DIR}")
message(STATUS "MIR directory:      ${MIR_DIR}")
message(STATUS "AsmJIT directory:   ${ASMJIT_DIR}")

# ==========================================================================
# Platform detection
# ==========================================================================
if(APPLE)
    set(PG_JITTER_SHARED_SUFFIX ".dylib")
    set(PG_JITTER_LINK_FLAGS -bundle -undefined dynamic_lookup)
elseif(UNIX)
    set(PG_JITTER_SHARED_SUFFIX ".so")
    set(PG_JITTER_LINK_FLAGS -shared)
else()
    set(PG_JITTER_SHARED_SUFFIX ".dll")
    set(PG_JITTER_LINK_FLAGS "")
endif()

# ==========================================================================
# Third-party libraries
# ==========================================================================

# ---------- sljit (static library from single source) ----------
add_library(sljit STATIC ${SLJIT_DIR}/sljit_src/sljitLir.c)
target_include_directories(sljit PUBLIC ${SLJIT_DIR}/sljit_src)
target_compile_options(sljit PRIVATE -w)

# ---------- MIR (static library) ----------
add_library(mir_lib STATIC ${MIR_DIR}/mir.c ${MIR_DIR}/mir-gen.c)
target_include_directories(mir_lib PUBLIC ${MIR_DIR})
target_compile_options(mir_lib PRIVATE -std=gnu11 -w)

find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(mir_lib PRIVATE Threads::Threads)
    target_compile_definitions(mir_lib PRIVATE MIR_PARALLEL_GEN)
endif()

# ---------- AsmJIT (via add_subdirectory) ----------
set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
set(ASMJIT_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${ASMJIT_DIR} ${CMAKE_BINARY_DIR}/asmjit)

# ==========================================================================
# Pre-compilation infrastructure (LLVM / c2mir)
# ==========================================================================
include(${CMAKE_SOURCE_DIR}/cmake/precompiled.cmake)

# ==========================================================================
# Pre-compiled deform templates
# ==========================================================================
set(DEFORM_GEN_SCRIPT "${CMAKE_SOURCE_DIR}/tools/gen_deform_templates.py")
set(DEFORM_TEMPLATE_C "${CMAKE_SOURCE_DIR}/src/pg_jit_deform_templates.c")
set(DEFORM_TEMPLATE_H "${CMAKE_SOURCE_DIR}/src/pg_jit_deform_templates.h")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_command(
        OUTPUT ${DEFORM_TEMPLATE_C}
        COMMAND ${Python3_EXECUTABLE} ${DEFORM_GEN_SCRIPT}
                --output-c ${DEFORM_TEMPLATE_C}
        DEPENDS ${DEFORM_GEN_SCRIPT}
        COMMENT "Generating pre-compiled deform templates (68 functions)"
    )
else()
    # Fallback: use pre-generated file (already in src/)
    message(STATUS "Python3 not found; using pre-generated deform templates")
endif()

# ==========================================================================
# Helper: configure a backend target with common settings
# ==========================================================================
function(pg_jitter_configure_target TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE ${PG_INCLUDEDIR_SERVER} src)
    pg_jitter_add_precompiled(${TARGET_NAME})
    set_target_properties(${TARGET_NAME} PROPERTIES
        PREFIX ""
        SUFFIX "${PG_JITTER_SHARED_SUFFIX}"
        C_VISIBILITY_PRESET default
    )
    target_link_options(${TARGET_NAME} PRIVATE ${PG_JITTER_LINK_FLAGS})
endfunction()

# ==========================================================================
# Backend targets
# ==========================================================================
set(COMMON_SRC src/pg_jitter_common.c src/pg_jit_funcs.c src/pg_jit_tier2_wrappers.c)

# ---------- pg_jitter_sljit ----------
add_library(pg_jitter_sljit MODULE ${COMMON_SRC} src/pg_jitter_sljit.c
            src/pg_jit_deform_templates.c)
target_link_libraries(pg_jitter_sljit PRIVATE sljit)
pg_jitter_configure_target(pg_jitter_sljit)

# ---------- pg_jitter_asmjit ----------
add_library(pg_jitter_asmjit MODULE ${COMMON_SRC} src/pg_jitter_asmjit.cpp
            src/pg_jit_deform_templates.c)
target_link_libraries(pg_jitter_asmjit PRIVATE asmjit::asmjit)
pg_jitter_configure_target(pg_jitter_asmjit)
set_target_properties(pg_jitter_asmjit PROPERTIES CXX_VISIBILITY_PRESET default)

# ---------- pg_jitter_mir ----------
add_library(pg_jitter_mir MODULE ${COMMON_SRC} src/pg_jitter_mir.c)
target_link_libraries(pg_jitter_mir PRIVATE mir_lib)
pg_jitter_configure_target(pg_jitter_mir)

# ==========================================================================
# Install
# ==========================================================================
foreach(_tgt pg_jitter_sljit pg_jitter_asmjit pg_jitter_mir)
    # Use install(FILES) on the built target to avoid cmake MODULE install issues.
    # $<TARGET_FILE:...> resolves to the full path of the built shared library.
    install(FILES $<TARGET_FILE:${_tgt}> DESTINATION ${PG_JITTER_INSTALL_DIR})
endforeach()
