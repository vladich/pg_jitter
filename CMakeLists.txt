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
#            -DPG_JITTER_BACKENDS="sljit;asmjit;mir"              (optional)
#            -DPG_JITTER_USE_LLVM=ON                              (optional)
#            -DPG_JITTER_USE_C2MIR=ON                             (optional)
#            -DPG_JITTER_INSTALL_DIR=/custom/install/path         (optional)
#

# ---------- Backend selection ----------
# AsmJIT only supports arm64 and x86_64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|x86_64|AMD64")
    set(_default_backends "sljit;asmjit;mir")
else()
    set(_default_backends "sljit;mir")
endif()
set(PG_JITTER_BACKENDS "${_default_backends}" CACHE STRING
    "Semicolon-separated list of backends to build (sljit, asmjit, mir)")

# Parse list into booleans for use below
list(FIND PG_JITTER_BACKENDS "sljit"  _idx)
if(_idx GREATER_EQUAL 0)
    set(PG_JITTER_BUILD_SLJIT ON)
else()
    set(PG_JITTER_BUILD_SLJIT OFF)
endif()
list(FIND PG_JITTER_BACKENDS "asmjit" _idx)
if(_idx GREATER_EQUAL 0)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|x86_64|AMD64")
        message(FATAL_ERROR
            "AsmJIT requires arm64 or x86_64 (detected: ${CMAKE_SYSTEM_PROCESSOR})")
    endif()
    set(PG_JITTER_BUILD_ASMJIT ON)
else()
    set(PG_JITTER_BUILD_ASMJIT OFF)
endif()
list(FIND PG_JITTER_BACKENDS "mir"    _idx)
if(_idx GREATER_EQUAL 0)
    set(PG_JITTER_BUILD_MIR ON)
else()
    set(PG_JITTER_BUILD_MIR OFF)
endif()

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

# Validate dependency directories (only for enabled backends)
if(PG_JITTER_BUILD_SLJIT)
    if(NOT EXISTS "${SLJIT_DIR}/sljit_src/sljitLir.c")
        message(FATAL_ERROR "sljit not found at ${SLJIT_DIR}. Set -DSLJIT_DIR=/path/to/sljit")
    endif()
    message(STATUS "sljit directory:    ${SLJIT_DIR}")
endif()
if(PG_JITTER_BUILD_MIR OR PG_JITTER_USE_C2MIR)
    if(NOT EXISTS "${MIR_DIR}/mir.c")
        message(FATAL_ERROR "MIR not found at ${MIR_DIR}. Set -DMIR_DIR=/path/to/mir")
    endif()
    message(STATUS "MIR directory:      ${MIR_DIR}")
endif()
if(PG_JITTER_BUILD_ASMJIT)
    if(NOT EXISTS "${ASMJIT_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "AsmJIT not found at ${ASMJIT_DIR}. Set -DASMJIT_DIR=/path/to/asmjit")
    endif()
    message(STATUS "AsmJIT directory:   ${ASMJIT_DIR}")
endif()

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
# Third-party libraries (only for enabled backends)
# ==========================================================================
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------- sljit (static library from single source) ----------
if(PG_JITTER_BUILD_SLJIT)
    add_library(sljit STATIC ${SLJIT_DIR}/sljit_src/sljitLir.c)
    target_include_directories(sljit PUBLIC ${SLJIT_DIR}/sljit_src)
    target_compile_options(sljit PRIVATE -w)
endif()

# ---------- MIR (static library) ----------
if(PG_JITTER_BUILD_MIR OR PG_JITTER_USE_C2MIR)
    add_library(mir_lib STATIC ${MIR_DIR}/mir.c ${MIR_DIR}/mir-gen.c)
    target_include_directories(mir_lib PUBLIC ${MIR_DIR})
    target_compile_options(mir_lib PRIVATE -std=gnu11 -w)

    find_package(Threads)
    if(Threads_FOUND)
        target_link_libraries(mir_lib PRIVATE Threads::Threads)
        target_compile_definitions(mir_lib PRIVATE MIR_PARALLEL_GEN)
    endif()
endif()

# ---------- AsmJIT (via add_subdirectory) ----------
if(PG_JITTER_BUILD_ASMJIT)
    set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
    set(ASMJIT_NO_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory(${ASMJIT_DIR} ${CMAKE_BINARY_DIR}/asmjit)
endif()

# ==========================================================================
# Pre-compilation infrastructure (LLVM / c2mir)
# ==========================================================================
include(${CMAKE_SOURCE_DIR}/cmake/precompiled.cmake)

# ==========================================================================
# Pre-compiled deform templates
# ==========================================================================
if(PG_JITTER_BUILD_SLJIT OR PG_JITTER_BUILD_ASMJIT)
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

# Collect targets for install
set(PG_JITTER_TARGETS "")

# ---------- pg_jitter_sljit ----------
if(PG_JITTER_BUILD_SLJIT)
    add_library(pg_jitter_sljit MODULE ${COMMON_SRC} src/pg_jitter_sljit.c
                src/pg_jit_deform_templates.c)
    target_link_libraries(pg_jitter_sljit PRIVATE sljit)
    pg_jitter_configure_target(pg_jitter_sljit)
    list(APPEND PG_JITTER_TARGETS pg_jitter_sljit)
endif()

# ---------- pg_jitter_asmjit ----------
if(PG_JITTER_BUILD_ASMJIT)
    add_library(pg_jitter_asmjit MODULE ${COMMON_SRC} src/pg_jitter_asmjit.cpp
                src/pg_jit_deform_templates.c)
    target_link_libraries(pg_jitter_asmjit PRIVATE asmjit::asmjit)
    pg_jitter_configure_target(pg_jitter_asmjit)
    set_target_properties(pg_jitter_asmjit PROPERTIES CXX_VISIBILITY_PRESET default)
    list(APPEND PG_JITTER_TARGETS pg_jitter_asmjit)
endif()

# ---------- pg_jitter_mir ----------
if(PG_JITTER_BUILD_MIR)
    add_library(pg_jitter_mir MODULE ${COMMON_SRC} src/pg_jitter_mir.c)
    target_link_libraries(pg_jitter_mir PRIVATE mir_lib)
    pg_jitter_configure_target(pg_jitter_mir)
    list(APPEND PG_JITTER_TARGETS pg_jitter_mir)
endif()

# ---------- pg_jitter meta-provider (always built) ----------
add_library(pg_jitter MODULE src/pg_jitter_meta.c)
target_include_directories(pg_jitter PRIVATE ${PG_INCLUDEDIR_SERVER} src)
set_target_properties(pg_jitter PROPERTIES
    PREFIX ""
    SUFFIX "${PG_JITTER_SHARED_SUFFIX}"
    C_VISIBILITY_PRESET default
)
target_link_options(pg_jitter PRIVATE ${PG_JITTER_LINK_FLAGS})
list(APPEND PG_JITTER_TARGETS pg_jitter)

# ==========================================================================
# Build summary
# ==========================================================================
message(STATUS "")
message(STATUS "pg_jitter backends:")
message(STATUS "  sljit:  ${PG_JITTER_BUILD_SLJIT}")
message(STATUS "  asmjit: ${PG_JITTER_BUILD_ASMJIT}")
message(STATUS "  mir:    ${PG_JITTER_BUILD_MIR}")
message(STATUS "  meta:   always")
message(STATUS "")

# ==========================================================================
# Install
# ==========================================================================
foreach(_tgt ${PG_JITTER_TARGETS})
    # Use install(FILES) on the built target to avoid cmake MODULE install issues.
    # $<TARGET_FILE:...> resolves to the full path of the built shared library.
    install(FILES $<TARGET_FILE:${_tgt}> DESTINATION ${PG_JITTER_INSTALL_DIR})
endforeach()
