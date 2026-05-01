cmake_minimum_required(VERSION 3.16)
project(pg_jitter C CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (default: Release)" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
# PG17+ headers use C99 designated initializers in inline functions
# (e.g. storage/fd.h). MSVC requires C++20 for designated initializer support.
if(MSVC)
    set(CMAKE_CXX_STANDARD 20)
else()
    set(CMAKE_CXX_STANDARD 17)
endif()

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
#            -DPG_JITTER_INSTALL_DIR=/custom/install/path         (optional)
#

# ---------- Backend selection ----------
# AsmJIT only supports arm64 and x86_64/amd64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|x86_64|AMD64|amd64")
    set(_default_backends "sljit;asmjit;mir")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "s390x")
    set(_default_backends "sljit")
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
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|x86_64|AMD64|amd64")
        message(FATAL_ERROR
            "AsmJIT requires arm64 or x86_64/amd64 (detected: ${CMAKE_SYSTEM_PROCESSOR})")
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
set(PG_JITTER_NEEDS_SLJIT OFF)
if(PG_JITTER_BUILD_SLJIT OR PG_JITTER_BUILD_ASMJIT OR PG_JITTER_BUILD_MIR)
    set(PG_JITTER_NEEDS_SLJIT ON)
endif()

# ---------- PostgreSQL paths via pg_config ----------
set(PG_CONFIG "pg_config" CACHE FILEPATH "Path to pg_config executable")

execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _pg_config_result)
if(NOT _pg_config_result EQUAL 0)
    message(FATAL_ERROR "pg_config failed. Set -DPG_CONFIG=/path/to/pg_config")
endif()
execute_process(COMMAND ${PG_CONFIG} --includedir
    OUTPUT_VARIABLE PG_INCLUDEDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --libdir
    OUTPUT_VARIABLE PG_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

# Extract PG major version for feature gating
execute_process(COMMAND ${PG_CONFIG} --version
    OUTPUT_VARIABLE _pg_ver_str OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "[0-9]+" PG_MAJOR_VERSION "${_pg_ver_str}")
math(EXPR PG_VERSION_NUM "${PG_MAJOR_VERSION} * 10000")
message(STATUS "PG version: ${PG_MAJOR_VERSION} (${PG_VERSION_NUM})")

# Extract CPPFLAGS from pg_config for additional include paths (e.g. libintl)
# Note: Windows binary installs (PG 14-16) return "not recorded" here.
execute_process(COMMAND ${PG_CONFIG} --cppflags
    OUTPUT_VARIABLE PG_CPPFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
if(PG_CPPFLAGS)
    separate_arguments(_pg_cppflags_list NATIVE_COMMAND "${PG_CPPFLAGS}")
    foreach(_flag IN LISTS _pg_cppflags_list)
        if(_flag MATCHES "^-I(.+)")
            list(APPEND PG_EXTRA_INCLUDE_DIRS "${CMAKE_MATCH_1}")
        elseif(_flag MATCHES "^[-/]")
            string(APPEND CMAKE_C_FLAGS " ${_flag}")
            string(APPEND CMAKE_CXX_FLAGS " ${_flag}")
        endif()
    endforeach()
endif()

message(STATUS "PG server includes: ${PG_INCLUDEDIR_SERVER}")
message(STATUS "PG includes:        ${PG_INCLUDEDIR}")
message(STATUS "PG pkglibdir:       ${PG_PKGLIBDIR}")
message(STATUS "PG libdir:          ${PG_LIBDIR}")

# ---------- Windows: find postgres import library ----------
if(WIN32)
    find_library(POSTGRES_LIB
        NAMES postgres postgres.exe
        PATHS "${PG_LIBDIR}" "${PG_PKGLIBDIR}"
        NO_DEFAULT_PATH)
    if(NOT POSTGRES_LIB)
        message(FATAL_ERROR "postgres.lib not found in ${PG_LIBDIR} or ${PG_PKGLIBDIR}")
    endif()
    message(STATUS "postgres.lib:       ${POSTGRES_LIB}")
endif()

# ---------- Installation directory ----------
# By default, install into PG's pkglibdir. Override with -DPG_JITTER_INSTALL_DIR.
set(PG_JITTER_INSTALL_DIR "${PG_PKGLIBDIR}" CACHE PATH
    "Installation directory for pg_jitter shared libraries")
message(STATUS "Install directory:  ${PG_JITTER_INSTALL_DIR}")

set(PG_JITTER_EXTRA_INSTALL_DIRS "")
set(_pg_jitter_lib_postgresql "${PG_LIBDIR}/postgresql")
if(IS_DIRECTORY "${_pg_jitter_lib_postgresql}"
   AND NOT "${_pg_jitter_lib_postgresql}" STREQUAL "${PG_JITTER_INSTALL_DIR}")
    list(APPEND PG_JITTER_EXTRA_INSTALL_DIRS "${_pg_jitter_lib_postgresql}")
    message(STATUS "Extra install dir:  ${_pg_jitter_lib_postgresql}")
endif()

# ---------- Dependency directories ----------
set(SLJIT_DIR "${CMAKE_SOURCE_DIR}/../sljit" CACHE PATH
    "Path to sljit source directory")
set(ASMJIT_DIR "${CMAKE_SOURCE_DIR}/../asmjit" CACHE PATH
    "Path to AsmJIT source directory")

set(_pg_jitter_patched_mir_dir "${CMAKE_SOURCE_DIR}/../mir-patched")
set(_pg_jitter_default_mir_dir "${_pg_jitter_patched_mir_dir}")
set(_pg_jitter_mir_default_fallback OFF)
if(NOT EXISTS "${_pg_jitter_default_mir_dir}/mir.c"
   AND EXISTS "${CMAKE_SOURCE_DIR}/../mir/mir.c")
    set(_pg_jitter_default_mir_dir "${CMAKE_SOURCE_DIR}/../mir")
    set(_pg_jitter_mir_default_fallback ON)
endif()
set(MIR_DIR "${_pg_jitter_default_mir_dir}" CACHE PATH
    "Path to MIR source directory")
if("${MIR_DIR}" STREQUAL "${_pg_jitter_patched_mir_dir}"
   AND NOT EXISTS "${MIR_DIR}/mir.c"
   AND EXISTS "${_pg_jitter_default_mir_dir}/mir.c")
    set(MIR_DIR "${_pg_jitter_default_mir_dir}" CACHE PATH
        "Path to MIR source directory" FORCE)
    set(_pg_jitter_mir_default_fallback ON)
endif()
if(_pg_jitter_mir_default_fallback AND PG_JITTER_BUILD_MIR)
    message(WARNING
        "MIR defaulted to ../mir because ../mir-patched was not found. "
        "../mir must be the patched MIR tree; set -DMIR_DIR explicitly "
        "if your checkout lives elsewhere.")
endif()

set(_pg_jitter_default_stringzilla_dir "${CMAKE_SOURCE_DIR}/../stringzilla")
if(NOT EXISTS "${_pg_jitter_default_stringzilla_dir}/include/stringzilla/stringzilla.h"
   AND EXISTS "${CMAKE_SOURCE_DIR}/../StringZilla/include/stringzilla/stringzilla.h")
    set(_pg_jitter_default_stringzilla_dir "${CMAKE_SOURCE_DIR}/../StringZilla")
endif()
set(STRINGZILLA_DIR "${_pg_jitter_default_stringzilla_dir}" CACHE PATH
    "Path to StringZilla source directory (header-only)")
set(PCRE2_DIR "${CMAKE_SOURCE_DIR}/../pcre2" CACHE PATH
    "Path to PCRE2 source directory")
option(WITH_PCRE2 "Enable PCRE2 Unicode-aware regex/LIKE acceleration" ON)
set(YYJSON_DIR "${CMAKE_SOURCE_DIR}/../yyjson" CACHE PATH
    "Path to yyjson source directory")
option(WITH_YYJSON "Enable yyjson JSON parsing acceleration" ON)


# Validate dependency directories (only for enabled backends)
if(PG_JITTER_NEEDS_SLJIT)
    if(NOT EXISTS "${SLJIT_DIR}/sljit_src/sljitLir.c")
        message(FATAL_ERROR "sljit not found at ${SLJIT_DIR}. Set -DSLJIT_DIR=/path/to/sljit")
    endif()
    message(STATUS "sljit directory:    ${SLJIT_DIR}")
endif()
if(PG_JITTER_BUILD_MIR)
    if(NOT EXISTS "${MIR_DIR}/mir.c")
        message(FATAL_ERROR "MIR not found at ${MIR_DIR}. Set -DMIR_DIR=/path/to/mir")
    endif()
    if(NOT EXISTS "${MIR_DIR}/mir.h")
        message(FATAL_ERROR "MIR header not found at ${MIR_DIR}/mir.h. Set -DMIR_DIR=/path/to/mir")
    endif()
    if(NOT EXISTS "${MIR_DIR}/mir-gen.c")
        message(FATAL_ERROR "MIR generator source not found at ${MIR_DIR}/mir-gen.c. Set -DMIR_DIR=/path/to/mir")
    endif()
    if(NOT EXISTS "${MIR_DIR}/mir-gen-x86_64.c")
        message(FATAL_ERROR "MIR x86_64 generator source not found at ${MIR_DIR}/mir-gen-x86_64.c. Set -DMIR_DIR=/path/to/mir")
    endif()
    file(READ "${MIR_DIR}/mir.h" _pg_jitter_mir_header)
    file(READ "${MIR_DIR}/mir-gen.c" _pg_jitter_mir_gen)
    file(READ "${MIR_DIR}/mir-gen-x86_64.c" _pg_jitter_mir_gen_x86_64)
    set(_pg_jitter_mir_text
        "${_pg_jitter_mir_header}\n${_pg_jitter_mir_gen}\n${_pg_jitter_mir_gen_x86_64}")
    if(NOT _pg_jitter_mir_text MATCHES "MIR_UEXT32")
        message(FATAL_ERROR
            "MIR at ${MIR_DIR} lacks patched unsigned-extension opcodes "
            "required by pg_jitter. Use the patched MIR tree or set -DMIR_DIR.")
    endif()
    if(NOT _pg_jitter_mir_text MATCHES "MIR_NO_RED_ZONE_ABI")
        message(FATAL_ERROR
            "MIR at ${MIR_DIR} lacks patched no-red-zone x86_64 support "
            "required by pg_jitter on Windows. Use the patched MIR tree or set -DMIR_DIR.")
    endif()
    if(EXISTS "${MIR_DIR}/CMakeLists.txt")
        file(READ "${MIR_DIR}/CMakeLists.txt" _pg_jitter_mir_cmake)
        if(NOT _pg_jitter_mir_cmake MATCHES "MIR_PARALLEL_GEN")
            message(FATAL_ERROR
                "MIR at ${MIR_DIR} lacks the patched parallel-generation build marker "
                "required by pg_jitter. Use the patched MIR tree or set -DMIR_DIR.")
        endif()
        unset(_pg_jitter_mir_cmake)
    endif()
    include(CheckCSourceCompiles)
    set(_pg_jitter_save_required_includes "${CMAKE_REQUIRED_INCLUDES}")
    set(_pg_jitter_save_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
    set(CMAKE_REQUIRED_INCLUDES "${MIR_DIR}")
    set(CMAKE_REQUIRED_DEFINITIONS "${_pg_jitter_save_required_definitions}")
    unset(PG_JITTER_MIR_API_PROBE_COMPILES CACHE)
    check_c_source_compiles("
        #include <mir.h>
        int main(void)
        {
            MIR_insn_code_t code = MIR_UEXT32;
            MIR_type_t type = MIR_T_U32;
            return (int) code + (int) type;
        }"
        PG_JITTER_MIR_API_PROBE_COMPILES)
    set(CMAKE_REQUIRED_INCLUDES "${_pg_jitter_save_required_includes}")
    set(CMAKE_REQUIRED_DEFINITIONS "${_pg_jitter_save_required_definitions}")
    if(NOT PG_JITTER_MIR_API_PROBE_COMPILES)
        message(FATAL_ERROR
            "MIR at ${MIR_DIR} failed pg_jitter's patched API compile probe. "
            "Use the patched MIR tree or set -DMIR_DIR.")
    endif()
    unset(_pg_jitter_mir_header)
    unset(_pg_jitter_mir_gen)
    unset(_pg_jitter_mir_gen_x86_64)
    unset(_pg_jitter_mir_text)
    message(STATUS "MIR directory:      ${MIR_DIR}")
endif()
if(PG_JITTER_BUILD_ASMJIT)
    if(NOT EXISTS "${ASMJIT_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "AsmJIT not found at ${ASMJIT_DIR}. Set -DASMJIT_DIR=/path/to/asmjit")
    endif()
    message(STATUS "AsmJIT directory:   ${ASMJIT_DIR}")
endif()

if(NOT EXISTS "${STRINGZILLA_DIR}/include/stringzilla/stringzilla.h")
    set(_pg_jitter_stringzilla_candidates
        "${CMAKE_SOURCE_DIR}/../StringZilla"
        "${CMAKE_SOURCE_DIR}/../stringzilla")
    foreach(_candidate IN LISTS _pg_jitter_stringzilla_candidates)
        if(EXISTS "${_candidate}/include/stringzilla/stringzilla.h")
            set(STRINGZILLA_DIR "${_candidate}" CACHE PATH
                "Path to StringZilla source directory (header-only)" FORCE)
            break()
        endif()
    endforeach()
endif()

if(NOT EXISTS "${STRINGZILLA_DIR}/include/stringzilla/stringzilla.h")
    message(FATAL_ERROR
        "StringZilla not found at ${STRINGZILLA_DIR}. "
        "Set -DSTRINGZILLA_DIR=/path/to/StringZilla")
endif()
message(STATUS "StringZilla:        ${STRINGZILLA_DIR}")

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

function(pg_jitter_apply_exports TARGET_NAME)
    set(_symbols ${ARGN})
    set(_export_dir "${CMAKE_CURRENT_BINARY_DIR}/exports")
    file(MAKE_DIRECTORY "${_export_dir}")

    if(APPLE)
        set(_export_file "${_export_dir}/${TARGET_NAME}.exports")
        file(WRITE "${_export_file}" "")
        foreach(_sym IN LISTS _symbols)
            file(APPEND "${_export_file}" "_${_sym}\n")
        endforeach()
        target_link_options(${TARGET_NAME} PRIVATE
            "-Wl,-exported_symbols_list,${_export_file}")
    elseif(UNIX)
        set(_export_file "${_export_dir}/${TARGET_NAME}.map")
        file(WRITE "${_export_file}" "PG_JITTER_1 {\n  global:\n")
        foreach(_sym IN LISTS _symbols)
            file(APPEND "${_export_file}" "    ${_sym};\n")
        endforeach()
        file(APPEND "${_export_file}" "  local:\n    *;\n};\n")
        target_link_options(${TARGET_NAME} PRIVATE
            "-Wl,--version-script=${_export_file}")
    endif()
endfunction()

set(PG_JITTER_PROVIDER_EXPORTS
    Pg_magic_func
    _PG_jit_provider_init
    pg_jitter_deform_dispatch_reset_fastpath
    pg_jitter_reset_third_party_counters
    pg_finfo_pg_jitter_reset_third_party_counters
    pg_jitter_third_party_counter
    pg_finfo_pg_jitter_third_party_counter)

set(PG_JITTER_META_EXPORTS
    Pg_magic_func
    _PG_jit_provider_init
    pg_jitter_current_backend
    pg_finfo_pg_jitter_current_backend
    pg_jitter_adaptive_stats
    pg_finfo_pg_jitter_adaptive_stats
    pg_jitter_adaptive_stats_reset
    pg_finfo_pg_jitter_adaptive_stats_reset)

# ==========================================================================
# Third-party libraries (only for enabled backends)
# ==========================================================================
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------- sljit (static library from single source) ----------
if(PG_JITTER_NEEDS_SLJIT)
    add_library(sljit STATIC ${SLJIT_DIR}/sljit_src/sljitLir.c)
    target_include_directories(sljit PUBLIC ${SLJIT_DIR}/sljit_src)
    set_target_properties(sljit PROPERTIES
        C_VISIBILITY_PRESET hidden)
    target_compile_options(sljit PRIVATE
        $<$<C_COMPILER_ID:MSVC>:/w>
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
endif()

# ---------- MIR (static library) ----------
if(PG_JITTER_BUILD_MIR)
    add_library(mir_lib STATIC ${MIR_DIR}/mir.c ${MIR_DIR}/mir-gen.c)
    target_include_directories(mir_lib PUBLIC ${MIR_DIR})
    set_target_properties(mir_lib PROPERTIES
        C_VISIBILITY_PRESET hidden)
    target_compile_options(mir_lib PRIVATE
        $<$<C_COMPILER_ID:MSVC>:/w>
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-std=gnu11 -w>)

    find_package(Threads)
    if(Threads_FOUND)
        target_link_libraries(mir_lib PRIVATE Threads::Threads)
        target_compile_definitions(mir_lib PRIVATE MIR_PARALLEL_GEN)
    endif()

    # Windows x64 has no red zone — MIR must not store below RSP
    if(WIN32)
        target_compile_definitions(mir_lib PRIVATE MIR_NO_RED_ZONE_ABI)
    endif()
endif()

# ---------- AsmJIT (via add_subdirectory) ----------
if(PG_JITTER_BUILD_ASMJIT)
    set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
    set(ASMJIT_NO_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory(${ASMJIT_DIR} ${CMAKE_BINARY_DIR}/asmjit)
    if(TARGET asmjit)
        set_target_properties(asmjit PROPERTIES
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
    endif()
endif()

# ---------- PCRE2 (optional, static library with JIT) ----------
set(PG_JITTER_HAVE_PCRE2 OFF)
if(WITH_PCRE2 AND NOT WIN32)
    # PCRE2 CMake puts .a directly in build/, not build/lib/
    set(_pcre2_lib "${PCRE2_DIR}/build/libpcre2-8.a")
    set(_pcre2_hdr "${PCRE2_DIR}/build/interface/pcre2.h")
    if(EXISTS "${_pcre2_lib}" AND EXISTS "${_pcre2_hdr}")
        set(PG_JITTER_HAVE_PCRE2 ON)
        set(PCRE2_INCLUDE_DIR "${PCRE2_DIR}/build/interface")
        set(PCRE2_LIBRARY_FOR_CHECK "${_pcre2_lib}")
        add_library(pcre2_lib STATIC IMPORTED)
        set_target_properties(pcre2_lib PROPERTIES
            IMPORTED_LOCATION "${_pcre2_lib}")
        message(STATUS "PCRE2:              ${_pcre2_lib}")
    else()
        # Try system-installed pcre2
        find_path(_pcre2_sys_hdr NAMES pcre2.h)
        if(_pcre2_sys_hdr)
            get_filename_component(_pcre2_sys_prefix "${_pcre2_sys_hdr}" DIRECTORY)
            find_library(_pcre2_hdr_lib NAMES pcre2-8
                PATHS "${_pcre2_sys_prefix}/lib" "${_pcre2_sys_prefix}/lib64"
                NO_DEFAULT_PATH)
        endif()
        if(_pcre2_hdr_lib)
            set(_pcre2_sys_lib "${_pcre2_hdr_lib}")
        else()
            find_library(_pcre2_sys_lib NAMES pcre2-8)
        endif()
        if(_pcre2_sys_hdr AND _pcre2_sys_lib)
            set(PG_JITTER_HAVE_PCRE2 ON)
            set(PCRE2_INCLUDE_DIR "${_pcre2_sys_hdr}")
            set(PCRE2_LIBRARY_FOR_CHECK "${_pcre2_sys_lib}")
            if(_pcre2_sys_lib MATCHES "\\.a$")
                add_library(pcre2_lib STATIC IMPORTED)
            else()
                add_library(pcre2_lib SHARED IMPORTED)
            endif()
            set_target_properties(pcre2_lib PROPERTIES
                IMPORTED_LOCATION "${_pcre2_sys_lib}")
            message(STATUS "PCRE2:              ${_pcre2_sys_lib} (system)")
        else()
            message(STATUS "PCRE2:              not found (build from source or install libpcre2-dev)")
        endif()
    endif()
endif()
set(PG_JITTER_HAVE_PCRE2_JIT_FAST_CONTEXT_FREE OFF)
if(PG_JITTER_HAVE_PCRE2)
    include(CheckCSourceCompiles)
    set(_pg_jitter_save_required_includes "${CMAKE_REQUIRED_INCLUDES}")
    set(_pg_jitter_save_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
    set(_pg_jitter_save_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_INCLUDES "${PCRE2_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_DEFINITIONS
        ${_pg_jitter_save_required_definitions}
        -DPCRE2_CODE_UNIT_WIDTH=8)
    set(CMAKE_REQUIRED_LIBRARIES "${PCRE2_LIBRARY_FOR_CHECK}")
    check_c_source_compiles("
        #define PCRE2_CODE_UNIT_WIDTH 8
        #include <pcre2.h>
        int main(void)
        {
        #ifndef PCRE2_JIT_FAST_API
        # error PCRE2_JIT_FAST_API is unavailable
        #endif
            pcre2_code *code = 0;
            pcre2_match_data *match_data = 0;
            pcre2_jit_stack *jit_stack = 0;
            pcre2_jit_fast_context *ctx =
                pcre2_jit_fast_context_create(code, PCRE2_JIT_COMPLETE,
                                              match_data, jit_stack);
            (void)pcre2_jit_fast_match(ctx, (PCRE2_SPTR)\"\", 0, 0, 0);
            pcre2_jit_fast_context_free(ctx);
            return 0;
        }"
        PG_JITTER_HAVE_PCRE2_JIT_FAST_CONTEXT_FREE)
    set(CMAKE_REQUIRED_INCLUDES "${_pg_jitter_save_required_includes}")
    set(CMAKE_REQUIRED_DEFINITIONS "${_pg_jitter_save_required_definitions}")
    set(CMAKE_REQUIRED_LIBRARIES "${_pg_jitter_save_required_libraries}")
    if(PG_JITTER_HAVE_PCRE2_JIT_FAST_CONTEXT_FREE)
        message(STATUS "PCRE2 fast JIT:     enabled")
    else()
        message(STATUS "PCRE2 fast JIT:     disabled (fast API unavailable or not linkable)")
    endif()
endif()

# ---------- yyjson (optional C static library) ----------
set(PG_JITTER_HAVE_YYJSON OFF)
if(WITH_YYJSON)
    set(_yj_hdr "${YYJSON_DIR}/src/yyjson.h")
    set(_yj_c "${YYJSON_DIR}/src/yyjson.c")
    if(EXISTS "${_yj_hdr}" AND EXISTS "${_yj_c}")
        file(READ "${_yj_hdr}" _yj_header_text)
        file(READ "${_yj_c}" _yj_source_text)
        if(NOT _yj_header_text MATCHES "yyjson_pg_text_unicode_validator"
           OR NOT _yj_header_text MATCHES "yyjson_read_pg_text_type_opts_err_ex"
           OR NOT _yj_source_text MATCHES "yyjson_read_pg_text_type_opts_err_ex"
           OR NOT _yj_source_text MATCHES "validate_unicode")
            message(FATAL_ERROR
                "yyjson at ${YYJSON_DIR} lacks pg_jitter's PostgreSQL JSON "
                "syntax helper yyjson_read_pg_text_type_opts_err_ex with "
                "Unicode-escape validation. "
                "Use the patched ../yyjson checkout or set -DYYJSON_DIR.")
        endif()
        set(PG_JITTER_YYJSON_PG_API_PROBE_TRUSTED OFF CACHE BOOL
            "Trust patched yyjson PostgreSQL syntax API when cross-compiling without an emulator")
        if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
            if(NOT PG_JITTER_YYJSON_PG_API_PROBE_TRUSTED)
                message(FATAL_ERROR
                    "yyjson at ${YYJSON_DIR} must pass pg_jitter's PostgreSQL "
                    "JSON syntax API runtime probe, but this is a cross build "
                    "without CMAKE_CROSSCOMPILING_EMULATOR. Configure with an "
                    "emulator or set -DPG_JITTER_YYJSON_PG_API_PROBE_TRUSTED=ON "
                    "only when the patched yyjson checkout has been verified.")
            endif()
            set(PG_JITTER_YYJSON_PG_API_PROBE_RUNS TRUE)
        else()
            include(CheckCSourceRuns)
            set(_pg_jitter_save_required_includes "${CMAKE_REQUIRED_INCLUDES}")
            set(_pg_jitter_save_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
            set(CMAKE_REQUIRED_INCLUDES "${YYJSON_DIR}/src")
            set(CMAKE_REQUIRED_DEFINITIONS
                ${_pg_jitter_save_required_definitions}
                -DYYJSON_READER_DEPTH_LIMIT=6401)
            unset(PG_JITTER_YYJSON_PG_API_PROBE_RUNS CACHE)
            check_c_source_runs([=[
            #include <stdint.h>
            #include <stdbool.h>
            #include "yyjson.c"
            static bool pg_jitter_yyjson_probe_validate(void *ctx,
                                                        uint32_t codepoint)
            {
                (void) ctx;
                return codepoint != 0x00E9;
            }
            int main(void)
            {
                yyjson_read_err err;
                static const char object_json[] =
                    {'{', '"', 'a', '"', ':', '1', '}', 0};
                static const char duplicate_json[] =
                    {'{', '"', 'a', '"', ':', '1', ',',
                     '"', 'a', '"', ':', '2', '}', 0};
                static const char unicode_json[] =
                    {0x22, 0x5C, 0x75, 0x30, 0x30, 0x45, 0x39, 0x22, 0};
                yyjson_alc invalid_alc = {0, 0, 0, 0};
                yyjson_pg_text_type object_type =
                    yyjson_read_pg_text_type_opts_err_ex(object_json,
                                                         sizeof(object_json) - 1,
                                                         true,
                                                         pg_jitter_yyjson_probe_validate,
                                                         0, 0, &err);
                yyjson_pg_text_type duplicate_type =
                    yyjson_read_pg_text_type_opts_err_ex(duplicate_json,
                                                         sizeof(duplicate_json) - 1,
                                                         true,
                                                         pg_jitter_yyjson_probe_validate,
                                                         0, 0, &err);
                yyjson_pg_text_type unicode_type =
                    yyjson_read_pg_text_type_opts_err_ex(unicode_json,
                                                         sizeof(unicode_json) - 1,
                                                         false,
                                                         pg_jitter_yyjson_probe_validate,
                                                         0, 0, &err);
                yyjson_pg_text_type invalid_allocator_type =
                    yyjson_read_pg_text_type_opts_err_ex(object_json,
                                                         sizeof(object_json) - 1,
                                                         true,
                                                         pg_jitter_yyjson_probe_validate,
                                                         0, &invalid_alc, &err);
                return object_type == YYJSON_PG_TEXT_OBJECT &&
                    duplicate_type == YYJSON_PG_TEXT_INVALID &&
                    unicode_type == YYJSON_PG_TEXT_INVALID &&
                    invalid_allocator_type == YYJSON_PG_TEXT_INVALID &&
                    err.code == YYJSON_READ_ERROR_INVALID_PARAMETER ? 0 : 1;
            }
            ]=]
                PG_JITTER_YYJSON_PG_API_PROBE_RUNS)
            set(CMAKE_REQUIRED_INCLUDES "${_pg_jitter_save_required_includes}")
            set(CMAKE_REQUIRED_DEFINITIONS "${_pg_jitter_save_required_definitions}")
        endif()
        if(NOT PG_JITTER_YYJSON_PG_API_PROBE_RUNS)
            message(FATAL_ERROR
                "yyjson at ${YYJSON_DIR} failed pg_jitter's PostgreSQL JSON "
                "syntax API runtime probe. Use the patched ../yyjson checkout "
                "or set -DYYJSON_DIR.")
        endif()
        unset(_yj_header_text)
        unset(_yj_source_text)
        set(PG_JITTER_HAVE_YYJSON ON)
        add_library(yyjson_lib STATIC
            ${_yj_c})
        target_include_directories(yyjson_lib PRIVATE
            ${YYJSON_DIR}/src)
        set_target_properties(yyjson_lib PROPERTIES
            C_STANDARD 11 POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET hidden)
        target_compile_definitions(yyjson_lib PRIVATE
            YYJSON_READER_DEPTH_LIMIT=6401)
        target_compile_options(yyjson_lib PRIVATE
            $<$<C_COMPILER_ID:MSVC>:/O2>
            $<$<NOT:$<C_COMPILER_ID:MSVC>>:-O2>)
        message(STATUS "yyjson:             ${_yj_hdr}")
    else()
        message(STATUS "yyjson:             not found at ${YYJSON_DIR}")
    endif()
endif()


# ==========================================================================
# Helper: configure a backend target with common settings
# ==========================================================================
function(pg_jitter_configure_target TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE ${PG_INCLUDEDIR_SERVER} ${PG_INCLUDEDIR} src ${PG_EXTRA_INCLUDE_DIRS}
        ${STRINGZILLA_DIR}/include)
    if(PG_VERSION_NUM LESS 160000 AND NOT MSVC)
        # PostgreSQL 14/15 headers still use the removed C++17 'register'
        # storage specifier in inline code paths reached from executor headers.
        target_compile_options(${TARGET_NAME} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>)
    endif()
    if(PG_JITTER_HAVE_PCRE2)
        target_include_directories(${TARGET_NAME} PRIVATE ${PCRE2_INCLUDE_DIR})
        target_compile_definitions(${TARGET_NAME} PRIVATE PG_JITTER_HAVE_PCRE2)
        if(PG_JITTER_HAVE_PCRE2_JIT_FAST_CONTEXT_FREE)
            target_compile_definitions(${TARGET_NAME} PRIVATE
                PG_JITTER_HAVE_PCRE2_JIT_FAST_CONTEXT_FREE)
        endif()
        target_link_libraries(${TARGET_NAME} PRIVATE pcre2_lib)
    endif()
    if(PG_JITTER_HAVE_YYJSON)
        target_sources(${TARGET_NAME} PRIVATE src/pg_jitter_yyjson.c)
        target_include_directories(${TARGET_NAME} PRIVATE ${YYJSON_DIR}/src)
        target_compile_definitions(${TARGET_NAME} PRIVATE PG_JITTER_HAVE_YYJSON)
        target_link_libraries(${TARGET_NAME} PRIVATE yyjson_lib)
        # Force linker to keep yyjson wrapper symbols referenced by JIT code.
        if(APPLE)
            target_link_options(${TARGET_NAME} PRIVATE
                -Wl,-u,_pg_jitter_yj_jsonb_in)
            if(PG_VERSION_NUM GREATER_EQUAL 160000)
                target_link_options(${TARGET_NAME} PRIVATE
                    -Wl,-u,_pg_jitter_yj_is_json_datum)
            endif()
        elseif(UNIX)
            target_link_options(${TARGET_NAME} PRIVATE
                -Wl,-u,pg_jitter_yj_jsonb_in)
            if(PG_VERSION_NUM GREATER_EQUAL 160000)
                target_link_options(${TARGET_NAME} PRIVATE
                    -Wl,-u,pg_jitter_yj_is_json_datum)
            endif()
        endif()
    endif()
    if(WIN32)
        # Our compat headers must come first to provide mmap/sysconf/etc.
        # that PG's own win32_msvc headers are missing.
        target_include_directories(${TARGET_NAME} BEFORE PRIVATE
            ${CMAKE_SOURCE_DIR}/compat/win32
            ${PG_INCLUDEDIR_SERVER}/port/win32_msvc
            ${PG_INCLUDEDIR_SERVER}/port/win32)
        target_link_libraries(${TARGET_NAME} PRIVATE ${POSTGRES_LIB} ws2_32)
    endif()
    set_target_properties(${TARGET_NAME} PROPERTIES
        PREFIX ""
        SUFFIX "${PG_JITTER_SHARED_SUFFIX}"
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    target_link_options(${TARGET_NAME} PRIVATE ${PG_JITTER_LINK_FLAGS})
    pg_jitter_apply_exports(${TARGET_NAME} ${PG_JITTER_PROVIDER_EXPORTS})
endfunction()

# ==========================================================================
# Backend targets
# ==========================================================================
set(COMMON_SRC src/pg_jitter_common.c src/pg_jit_funcs.c src/pg_jit_tier2_wrappers.c src/pg_jitter_simd.c src/pg_jitter_pcre2.c)

set(PG_JITTER_SIMD_COMPILE_OPTIONS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    if(MSVC)
        list(APPEND PG_JITTER_SIMD_COMPILE_OPTIONS /arch:AVX2)
    else()
        include(CheckCCompilerFlag)
        check_c_compiler_flag("-mavx2" PG_JITTER_CC_HAS_MAVX2)
        if(PG_JITTER_CC_HAS_MAVX2)
            list(APPEND PG_JITTER_SIMD_COMPILE_OPTIONS -mavx2)
        endif()
    endif()
endif()
if(PG_JITTER_SIMD_COMPILE_OPTIONS)
    set_source_files_properties(src/pg_jitter_simd.c PROPERTIES
        COMPILE_OPTIONS "${PG_JITTER_SIMD_COMPILE_OPTIONS}")
    message(STATUS "StringZilla SIMD C flags: ${PG_JITTER_SIMD_COMPILE_OPTIONS}")
endif()

# Collect targets for install
set(PG_JITTER_TARGETS "")

# ---------- pg_jitter_sljit ----------
if(PG_JITTER_BUILD_SLJIT)
    add_library(pg_jitter_sljit MODULE ${COMMON_SRC} src/pg_jitter_sljit.c
                src/pg_jitter_deform_jit.c)
    target_link_libraries(pg_jitter_sljit PRIVATE sljit)
    target_compile_definitions(pg_jitter_sljit PRIVATE PG_JITTER_HAVE_SLJIT_DEFORM)
    pg_jitter_configure_target(pg_jitter_sljit)
    list(APPEND PG_JITTER_TARGETS pg_jitter_sljit)
endif()

# ---------- pg_jitter_asmjit ----------
if(PG_JITTER_BUILD_ASMJIT)
    add_library(pg_jitter_asmjit MODULE ${COMMON_SRC} src/pg_jitter_asmjit.cpp
                src/pg_jitter_deform_jit.c)
    target_link_libraries(pg_jitter_asmjit PRIVATE asmjit::asmjit sljit)
    target_compile_definitions(pg_jitter_asmjit PRIVATE PG_JITTER_HAVE_SLJIT_DEFORM)
    pg_jitter_configure_target(pg_jitter_asmjit)
    list(APPEND PG_JITTER_TARGETS pg_jitter_asmjit)
endif()

# ---------- pg_jitter_mir ----------
if(PG_JITTER_BUILD_MIR)
    add_library(pg_jitter_mir MODULE ${COMMON_SRC} src/pg_jitter_mir.c
                src/pg_jitter_deform_jit.c)
    target_link_libraries(pg_jitter_mir PRIVATE mir_lib sljit)
    target_compile_definitions(pg_jitter_mir PRIVATE PG_JITTER_HAVE_SLJIT_DEFORM)
    pg_jitter_configure_target(pg_jitter_mir)
    list(APPEND PG_JITTER_TARGETS pg_jitter_mir)
endif()

# ---------- pg_jitter meta-provider (always built) ----------
add_library(pg_jitter MODULE src/pg_jitter_meta.c)
target_include_directories(pg_jitter PRIVATE ${PG_INCLUDEDIR_SERVER} ${PG_INCLUDEDIR} src ${PG_EXTRA_INCLUDE_DIRS})
if(WIN32)
    target_include_directories(pg_jitter BEFORE PRIVATE
        ${CMAKE_SOURCE_DIR}/compat/win32
        ${PG_INCLUDEDIR_SERVER}/port/win32_msvc
        ${PG_INCLUDEDIR_SERVER}/port/win32)
    target_link_libraries(pg_jitter PRIVATE ${POSTGRES_LIB} ws2_32)
endif()
set_target_properties(pg_jitter PROPERTIES
    PREFIX ""
    SUFFIX "${PG_JITTER_SHARED_SUFFIX}"
    C_VISIBILITY_PRESET hidden
)
target_link_options(pg_jitter PRIVATE ${PG_JITTER_LINK_FLAGS})
pg_jitter_apply_exports(pg_jitter ${PG_JITTER_META_EXPORTS})
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
    foreach(_extra_dir ${PG_JITTER_EXTRA_INSTALL_DIRS})
        install(FILES $<TARGET_FILE:${_tgt}> DESTINATION ${_extra_dir})
    endforeach()
endforeach()

# Install SQL function definitions
execute_process(COMMAND ${PG_CONFIG} --sharedir
    OUTPUT_VARIABLE PG_SHAREDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
install(FILES ${CMAKE_SOURCE_DIR}/sql/pg_jitter_functions.sql
    DESTINATION ${PG_SHAREDIR}/pg_jitter)
