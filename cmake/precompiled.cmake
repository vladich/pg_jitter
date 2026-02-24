#
# cmake/precompiled.cmake — Shared pre-compilation infrastructure for all backends
#
# Provides two independent, mutually exclusive pre-compilation pipelines:
#   1. LLVM pipeline (PG_JITTER_USE_LLVM): clang → .o → extract native blobs
#      - Produces architecture-specific native code bytes
#      - Tier 2: links with PG bitcode for deep inlining (requires PG --with-llvm)
#   2. c2mir pipeline (PG_JITTER_USE_C2MIR): c2mir → MIR_gen → native blobs
#      - Compiles at build time to native code for the host architecture
#      - No external toolchain required (only MIR source)
#
# Tier 2 C wrappers (pg_jit_tier2_wrappers.c) are always available regardless
# of which pipeline is selected. They provide pass-by-ref operations via
# DirectFunctionCall without requiring LLVM or c2mir.
#
# Usage: include() from CMakeLists.txt. Provides pg_jitter_add_precompiled()
# function to apply settings to each backend target.
#
# Variables set:
#   - PG_JITTER_HAVE_PRECOMPILED: native blobs available (LLVM pipeline)
#   - PG_JITTER_HAVE_TIER2: LLVM-optimized Tier 2 wrappers available
#   - PG_JITTER_HAVE_MIR_PRECOMPILED: native blobs available (c2mir pipeline)
#

# Guard against multiple inclusion
if(DEFINED _PG_JITTER_PRECOMPILED_INCLUDED)
    return()
endif()
set(_PG_JITTER_PRECOMPILED_INCLUDED TRUE)

# ---------- Options ----------
option(PG_JITTER_USE_LLVM  "Pre-compile functions with LLVM at build time" OFF)
option(PG_JITTER_USE_C2MIR "Pre-compile functions with c2mir at build time" OFF)

# Mutual exclusion check
if(PG_JITTER_USE_LLVM AND PG_JITTER_USE_C2MIR)
    message(FATAL_ERROR
        "PG_JITTER_USE_LLVM and PG_JITTER_USE_C2MIR are mutually exclusive.\n"
        "Choose one pre-compilation pipeline:\n"
        "  -DPG_JITTER_USE_LLVM=ON   (requires clang + llvm-objdump)\n"
        "  -DPG_JITTER_USE_C2MIR=ON  (requires MIR source only)")
endif()

# ---------- Detect target architecture ----------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(PRECOMPILED_ARCH "aarch64")
    if(APPLE)
        set(PRECOMPILED_TARGET "arm64-apple-darwin")
    else()
        set(PRECOMPILED_TARGET "aarch64-unknown-linux-gnu")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(PRECOMPILED_ARCH "x86_64")
    if(APPLE)
        set(PRECOMPILED_TARGET "x86_64-apple-darwin")
    else()
        set(PRECOMPILED_TARGET "x86_64-unknown-linux-gnu")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64|ppc64le")
    set(PRECOMPILED_ARCH "ppc64")
    set(PRECOMPILED_TARGET "powerpc64le-unknown-linux-gnu")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    set(PRECOMPILED_ARCH "riscv64")
    set(PRECOMPILED_TARGET "riscv64-unknown-linux-gnu")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "s390x")
    set(PRECOMPILED_ARCH "s390x")
    set(PRECOMPILED_TARGET "s390x-unknown-linux-gnu")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "loongarch64")
    set(PRECOMPILED_ARCH "loongarch64")
    set(PRECOMPILED_TARGET "loongarch64-unknown-linux-gnu")
else()
    set(PRECOMPILED_ARCH "unknown")
    set(PRECOMPILED_TARGET "")
endif()

# ==========================================================================
# Pipeline A: LLVM pre-compilation
# ==========================================================================
if(PG_JITTER_USE_LLVM)
    # Allow user to override LLVM tool paths
    set(LLVM_DIR "" CACHE PATH "Path to LLVM installation (bin/ containing clang, etc.)")

    # Build LLVM search hints from LLVM_DIR if provided
    set(_llvm_hints "")
    if(LLVM_DIR)
        list(APPEND _llvm_hints "${LLVM_DIR}/bin" "${LLVM_DIR}")
    endif()
    list(APPEND _llvm_hints
        /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@18/bin
        /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin /usr/lib/llvm-16/bin)

    find_program(CLANG_EXE NAMES clang clang-18 clang-17 clang-16
                 HINTS ${_llvm_hints})
    find_program(LLVM_OBJDUMP_EXE NAMES llvm-objdump llvm-objdump-18
                 llvm-objdump-17 llvm-objdump-16
                 HINTS ${_llvm_hints})
    find_program(LLVM_LINK_EXE NAMES llvm-link llvm-link-18 llvm-link-17
                 HINTS ${_llvm_hints})
    find_program(LLVM_OPT_EXE NAMES opt opt-18 opt-17
                 HINTS ${_llvm_hints})
    find_program(LLVM_LLC_EXE NAMES llc llc-18 llc-17
                 HINTS ${_llvm_hints})
    find_program(PYTHON3_EXE NAMES python3 python)

    # Part A: Tier 1 native blobs
    if(CLANG_EXE AND LLVM_OBJDUMP_EXE AND PYTHON3_EXE)
        set(PG_JITTER_HAVE_PRECOMPILED 1)
        message(STATUS "LLVM pre-compilation: ON (arch: ${PRECOMPILED_ARCH})")
        message(STATUS "  clang:        ${CLANG_EXE}")
        message(STATUS "  llvm-objdump: ${LLVM_OBJDUMP_EXE}")

        set(PRECOMPILED_OBJ "${CMAKE_BINARY_DIR}/pg_jit_funcs_precompiled.o")
        set(PRECOMPILED_HDR "${CMAKE_BINARY_DIR}/pg_jit_precompiled.h")
        set(EXTRACT_SCRIPT "${CMAKE_SOURCE_DIR}/tools/extract_inlines.py")

        add_custom_command(
            OUTPUT ${PRECOMPILED_OBJ}
            COMMAND ${CLANG_EXE} -O2 -c
                    -target ${PRECOMPILED_TARGET}
                    -I${PG_INCLUDEDIR_SERVER}
                    -I${CMAKE_SOURCE_DIR}/src
                    -DPG_JITTER_PRECOMPILE_ONLY
                    -o ${PRECOMPILED_OBJ}
                    ${CMAKE_SOURCE_DIR}/src/pg_jit_funcs.c
            DEPENDS ${CMAKE_SOURCE_DIR}/src/pg_jit_funcs.c
                    ${CMAKE_SOURCE_DIR}/src/pg_jit_funcs.h
            COMMENT "Compiling pg_jit_funcs.c with clang -O2 for pre-compiled blobs"
        )

        add_custom_command(
            OUTPUT ${PRECOMPILED_HDR}
            COMMAND ${PYTHON3_EXE} ${EXTRACT_SCRIPT}
                    --objdump ${LLVM_OBJDUMP_EXE}
                    --input ${PRECOMPILED_OBJ}
                    --output ${PRECOMPILED_HDR}
                    --arch ${PRECOMPILED_ARCH}
            DEPENDS ${PRECOMPILED_OBJ} ${EXTRACT_SCRIPT}
            COMMENT "Extracting pre-compiled inline blobs from pg_jit_funcs.o"
        )

        add_custom_target(precompiled_header DEPENDS ${PRECOMPILED_HDR})
    else()
        message(FATAL_ERROR
            "PG_JITTER_USE_LLVM=ON but required tools not found:\n"
            "  clang:        ${CLANG_EXE}\n"
            "  llvm-objdump: ${LLVM_OBJDUMP_EXE}\n"
            "  python3:      ${PYTHON3_EXE}\n"
            "Set -DLLVM_DIR=/path/to/llvm to help find them.")
    endif()

    # Part B: Tier 2 wrappers from PG bitcode
    if(PG_JITTER_HAVE_PRECOMPILED AND LLVM_LINK_EXE AND LLVM_OPT_EXE AND LLVM_LLC_EXE)
        execute_process(COMMAND ${PG_CONFIG} --libdir
            OUTPUT_VARIABLE PG_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(PG_BITCODE_DIR "${PG_LIBDIR}/bitcode/postgres")

        if(EXISTS "${PG_BITCODE_DIR}")
            set(PG_JITTER_HAVE_TIER2 1)
            set(GEN_TIER2_SCRIPT "${CMAKE_SOURCE_DIR}/tools/gen_tier2_wrappers.py")
            set(TIER2_IR "${CMAKE_BINARY_DIR}/tier2_wrappers.ll")
            set(TIER2_LINKED "${CMAKE_BINARY_DIR}/tier2_linked.bc")
            set(TIER2_OPT "${CMAKE_BINARY_DIR}/tier2_opt.bc")
            set(TIER2_OBJ "${CMAKE_BINARY_DIR}/tier2_wrappers.o")

            message(STATUS "  PG bitcode:   ${PG_BITCODE_DIR}")
            message(STATUS "  llvm-link:    ${LLVM_LINK_EXE}")
            message(STATUS "  opt:          ${LLVM_OPT_EXE}")
            message(STATUS "  llc:          ${LLVM_LLC_EXE}")
            message(STATUS "  Tier 2 pre-compilation: ON")

            add_custom_command(OUTPUT ${TIER2_IR}
                COMMAND ${PYTHON3_EXE} ${GEN_TIER2_SCRIPT}
                        --pg-include ${PG_INCLUDEDIR_SERVER}
                        --bitcode-dir ${PG_BITCODE_DIR}
                        --output ${TIER2_IR} --arch ${PRECOMPILED_ARCH}
                DEPENDS ${GEN_TIER2_SCRIPT}
                COMMENT "Generating LLVM IR wrappers for Tier 2 functions")
            add_custom_command(OUTPUT ${TIER2_LINKED}
                COMMAND ${LLVM_LINK_EXE} ${TIER2_IR}
                        ${PG_BITCODE_DIR}/utils/adt/numeric.bc
                        ${PG_BITCODE_DIR}/utils/adt/varlena.bc
                        ${PG_BITCODE_DIR}/utils/adt/uuid.bc
                        ${PG_BITCODE_DIR}/utils/adt/timestamp.bc
                        -o ${TIER2_LINKED}
                DEPENDS ${TIER2_IR}
                COMMENT "Linking Tier 2 wrappers with PG bitcode")
            add_custom_command(OUTPUT ${TIER2_OPT}
                COMMAND ${LLVM_OPT_EXE} -O2 ${TIER2_LINKED} -o ${TIER2_OPT}
                DEPENDS ${TIER2_LINKED}
                COMMENT "Optimizing Tier 2 wrappers")
            add_custom_command(OUTPUT ${TIER2_OBJ}
                COMMAND ${LLVM_LLC_EXE} -O2 -filetype=obj
                        -relocation-model=pic
                        ${TIER2_OPT} -o ${TIER2_OBJ}
                DEPENDS ${TIER2_OPT}
                COMMENT "Compiling Tier 2 wrappers to native code")
            add_custom_target(tier2_object DEPENDS ${TIER2_OBJ})
        else()
            message(STATUS "  PG bitcode not found at ${PG_BITCODE_DIR} — Tier 2 disabled")
        endif()
    endif()
endif()

# ==========================================================================
# Pipeline B: c2mir pre-compilation
# ==========================================================================
if(PG_JITTER_USE_C2MIR)
    # MIR_DIR is already defined as a CACHE variable in CMakeLists.txt

    set(MIR_PRECOMPILE_SRC "${CMAKE_SOURCE_DIR}/tools/mir_precompile.c")
    set(MIR_PRECOMPILED_HDR "${CMAKE_BINARY_DIR}/pg_jit_precompiled_mir.h")
    # Use simplified C source that c2mir can handle (no macOS math.h,
    # no PG __builtin_*_overflow). Same function signatures and semantics.
    set(MIR_JIT_FUNCS_SRC "${CMAKE_SOURCE_DIR}/tools/mir_jit_funcs.c")

    if(NOT EXISTS "${MIR_PRECOMPILE_SRC}")
        message(FATAL_ERROR "PG_JITTER_USE_C2MIR=ON but tools/mir_precompile.c not found")
    endif()

    set(PG_JITTER_HAVE_MIR_PRECOMPILED 1)
    message(STATUS "c2mir pre-compilation: ON (arch: ${PRECOMPILED_ARCH})")
    message(STATUS "  MIR source: ${MIR_DIR}")

    # Build the mir_precompile tool
    add_executable(mir_precompile ${MIR_PRECOMPILE_SRC})
    target_include_directories(mir_precompile PRIVATE ${MIR_DIR})
    target_link_libraries(mir_precompile PRIVATE mir_lib)
    # c2mir needs these source files compiled in
    target_sources(mir_precompile PRIVATE
        ${MIR_DIR}/c2mir/c2mir.c)
    target_include_directories(mir_precompile PRIVATE
        ${MIR_DIR}/c2mir)
    target_compile_options(mir_precompile PRIVATE -w)

    # Run at build time to generate native code blob header.
    # c2mir compiles to MIR IR, MIR_gen() produces native code at build time.
    # Uses mir_jit_funcs.c (c2mir-compatible) instead of pg_jit_funcs.c.
    add_custom_command(
        OUTPUT ${MIR_PRECOMPILED_HDR}
        COMMAND mir_precompile
                -o ${MIR_PRECOMPILED_HDR}
                ${MIR_JIT_FUNCS_SRC}
        DEPENDS mir_precompile
                ${MIR_JIT_FUNCS_SRC}
        COMMENT "Generating native pre-compiled blobs via c2mir + MIR_gen"
    )

    add_custom_target(mir_precompiled_header DEPENDS ${MIR_PRECOMPILED_HDR})
endif()

# ==========================================================================
# Summary
# ==========================================================================
if(NOT PG_JITTER_USE_LLVM AND NOT PG_JITTER_USE_C2MIR)
    message(STATUS "Pre-compilation: OFF (use -DPG_JITTER_USE_LLVM=ON or -DPG_JITTER_USE_C2MIR=ON)")
endif()

# ==========================================================================
# Helper function: apply precompiled settings to a backend target
# ==========================================================================
function(pg_jitter_add_precompiled TARGET_NAME)
    # LLVM native blobs
    if(PG_JITTER_HAVE_PRECOMPILED)
        target_compile_definitions(${TARGET_NAME} PRIVATE PG_JITTER_HAVE_PRECOMPILED)
        target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_BINARY_DIR})
        add_dependencies(${TARGET_NAME} precompiled_header)

        if(PG_JITTER_HAVE_TIER2)
            target_compile_definitions(${TARGET_NAME} PRIVATE PG_JITTER_HAVE_TIER2)
            target_sources(${TARGET_NAME} PRIVATE ${TIER2_OBJ})
            add_dependencies(${TARGET_NAME} tier2_object)
            set_source_files_properties(${TIER2_OBJ} PROPERTIES
                EXTERNAL_OBJECT TRUE GENERATED TRUE)
        endif()
    endif()

    # c2mir native blobs (all backends)
    if(PG_JITTER_HAVE_MIR_PRECOMPILED)
        target_compile_definitions(${TARGET_NAME} PRIVATE PG_JITTER_HAVE_MIR_PRECOMPILED)
        target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_BINARY_DIR})
        add_dependencies(${TARGET_NAME} mir_precompiled_header)
    endif()
endfunction()
