cmake_minimum_required(VERSION 3.16)
project(pg_jitter C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---------- PostgreSQL paths via pg_config ----------
set(PG_CONFIG "pg_config" CACHE FILEPATH "Path to pg_config")

execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS "PG server includes: ${PG_INCLUDEDIR_SERVER}")
message(STATUS "PG pkglibdir:       ${PG_PKGLIBDIR}")

# ---------- sljit (static library from single source) ----------
set(SLJIT_DIR "${CMAKE_SOURCE_DIR}/../sljit")
add_library(sljit STATIC ${SLJIT_DIR}/sljit_src/sljitLir.c)
target_include_directories(sljit PUBLIC ${SLJIT_DIR}/sljit_src)
target_compile_options(sljit PRIVATE -w)

# ---------- MIR (static library) ----------
set(MIR_DIR "${CMAKE_SOURCE_DIR}/../mir")
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
add_subdirectory(${CMAKE_SOURCE_DIR}/../asmjit ${CMAKE_BINARY_DIR}/asmjit)

# ---------- LLVM build-time pre-compilation (optional) ----------
option(PG_JITTER_USE_LLVM "Pre-compile functions with LLVM at build time" OFF)

if(PG_JITTER_USE_LLVM)
    find_program(CLANG_EXE NAMES clang clang-18 clang-17 clang-16
                 HINTS /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@18/bin
                       /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin)
    find_program(LLVM_OBJDUMP_EXE NAMES llvm-objdump llvm-objdump-18
                 llvm-objdump-17 llvm-objdump-16
                 HINTS /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@18/bin
                       /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin)
    find_program(LLVM_LINK_EXE NAMES llvm-link llvm-link-18 llvm-link-17
                 HINTS /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@18/bin
                       /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin)
    find_program(LLVM_OPT_EXE NAMES opt opt-18 opt-17
                 HINTS /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@18/bin
                       /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin)
    find_program(LLVM_LLC_EXE NAMES llc llc-18 llc-17
                 HINTS /opt/homebrew/opt/llvm/bin /opt/homebrew/opt/llvm@18/bin
                       /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin)
    find_program(PYTHON3_EXE NAMES python3 python)

    if(CLANG_EXE AND LLVM_OBJDUMP_EXE AND PYTHON3_EXE)
        set(PG_JITTER_HAVE_PRECOMPILED 1)
        message(STATUS "LLVM pre-compilation: ON")
        message(STATUS "  clang:        ${CLANG_EXE}")
        message(STATUS "  llvm-objdump: ${LLVM_OBJDUMP_EXE}")
        message(STATUS "  python3:      ${PYTHON3_EXE}")
    else()
        message(WARNING "PG_JITTER_USE_LLVM=ON but required tools not found:"
                        " clang=${CLANG_EXE} llvm-objdump=${LLVM_OBJDUMP_EXE}"
                        " python3=${PYTHON3_EXE}")
    endif()
endif()

# Detect target architecture for pre-compiled blobs
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(PRECOMPILED_ARCH "aarch64")
    set(PRECOMPILED_TARGET "arm64-apple-darwin")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(PRECOMPILED_ARCH "x86_64")
    set(PRECOMPILED_TARGET "x86_64-unknown-linux-gnu")
endif()

# ---------- Part A: Pre-compiled Tier 1 inline blobs ----------
if(PG_JITTER_HAVE_PRECOMPILED)
    set(PRECOMPILED_OBJ "${CMAKE_BINARY_DIR}/pg_jit_funcs_precompiled.o")
    set(PRECOMPILED_HDR "${CMAKE_BINARY_DIR}/pg_jit_precompiled.h")
    set(EXTRACT_SCRIPT "${CMAKE_SOURCE_DIR}/tools/extract_inlines.py")

    # Step 1: Compile pg_jit_funcs.c to object file with clang -O2
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

    # Step 2: Extract inline blobs via python script
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
endif()

# ---------- Part B: Tier 2 wrappers from PG bitcode ----------
if(PG_JITTER_HAVE_PRECOMPILED AND LLVM_LINK_EXE AND LLVM_OPT_EXE AND LLVM_LLC_EXE)
    # Find PG bitcode directory
    execute_process(COMMAND ${PG_CONFIG} --libdir
        OUTPUT_VARIABLE PG_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(PG_BITCODE_DIR "${PG_LIBDIR}/bitcode/postgres")
    set(PG_JITTER_HAVE_TIER2 0)

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

        # Step 1: Generate LLVM IR wrappers
        add_custom_command(
            OUTPUT ${TIER2_IR}
            COMMAND ${PYTHON3_EXE} ${GEN_TIER2_SCRIPT}
                    --pg-include ${PG_INCLUDEDIR_SERVER}
                    --bitcode-dir ${PG_BITCODE_DIR}
                    --output ${TIER2_IR}
                    --arch ${PRECOMPILED_ARCH}
            DEPENDS ${GEN_TIER2_SCRIPT}
            COMMENT "Generating LLVM IR wrappers for Tier 2 functions"
        )

        # Step 2: Link with PG bitcode modules
        add_custom_command(
            OUTPUT ${TIER2_LINKED}
            COMMAND ${LLVM_LINK_EXE}
                    ${TIER2_IR}
                    ${PG_BITCODE_DIR}/utils/adt/numeric.bc
                    ${PG_BITCODE_DIR}/utils/adt/varlena.bc
                    ${PG_BITCODE_DIR}/utils/adt/uuid.bc
                    ${PG_BITCODE_DIR}/utils/adt/timestamp.bc
                    -o ${TIER2_LINKED}
            DEPENDS ${TIER2_IR}
            COMMENT "Linking Tier 2 wrappers with PG bitcode"
        )

        # Step 3: Optimize
        add_custom_command(
            OUTPUT ${TIER2_OPT}
            COMMAND ${LLVM_OPT_EXE} -O2 ${TIER2_LINKED} -o ${TIER2_OPT}
            DEPENDS ${TIER2_LINKED}
            COMMENT "Optimizing Tier 2 wrappers"
        )

        # Step 4: Compile to native object
        add_custom_command(
            OUTPUT ${TIER2_OBJ}
            COMMAND ${LLVM_LLC_EXE} -O2 -filetype=obj
                    -relocation-model=pic
                    ${TIER2_OPT} -o ${TIER2_OBJ}
            DEPENDS ${TIER2_OPT}
            COMMENT "Compiling Tier 2 wrappers to native code"
        )

        add_custom_target(tier2_object DEPENDS ${TIER2_OBJ})
    else()
        message(STATUS "  PG bitcode not found at ${PG_BITCODE_DIR} — Tier 2 disabled")
    endif()
endif()

# ---------- Pre-compiled deform templates ----------
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

# ---------- Common source ----------
set(COMMON_SRC src/pg_jitter_common.c src/pg_jit_funcs.c)

# ---------- pg_jitter_sljit ----------
add_library(pg_jitter_sljit MODULE ${COMMON_SRC} src/pg_jitter_sljit.c
            src/pg_jit_deform_templates.c)
target_include_directories(pg_jitter_sljit PRIVATE ${PG_INCLUDEDIR_SERVER} src)
target_link_libraries(pg_jitter_sljit PRIVATE sljit)

# Add pre-compiled support if available
if(PG_JITTER_HAVE_PRECOMPILED)
    target_compile_definitions(pg_jitter_sljit PRIVATE PG_JITTER_HAVE_PRECOMPILED)
    target_include_directories(pg_jitter_sljit PRIVATE ${CMAKE_BINARY_DIR})
    add_dependencies(pg_jitter_sljit precompiled_header)

    if(PG_JITTER_HAVE_TIER2)
        target_compile_definitions(pg_jitter_sljit PRIVATE PG_JITTER_HAVE_TIER2)
        target_sources(pg_jitter_sljit PRIVATE ${TIER2_OBJ})
        add_dependencies(pg_jitter_sljit tier2_object)
        set_source_files_properties(${TIER2_OBJ} PROPERTIES
            EXTERNAL_OBJECT TRUE GENERATED TRUE)
    endif()
endif()
set_target_properties(pg_jitter_sljit PROPERTIES
    PREFIX ""
    SUFFIX ".dylib"
    C_VISIBILITY_PRESET default
)
target_link_options(pg_jitter_sljit PRIVATE -bundle -undefined dynamic_lookup)

# ---------- pg_jitter_asmjit ----------
add_library(pg_jitter_asmjit MODULE ${COMMON_SRC} src/pg_jitter_asmjit.cpp)
target_include_directories(pg_jitter_asmjit PRIVATE ${PG_INCLUDEDIR_SERVER} src)
target_link_libraries(pg_jitter_asmjit PRIVATE asmjit::asmjit)
set_target_properties(pg_jitter_asmjit PROPERTIES
    PREFIX ""
    SUFFIX ".dylib"
    CXX_VISIBILITY_PRESET default
    C_VISIBILITY_PRESET default
)
target_link_options(pg_jitter_asmjit PRIVATE -bundle -undefined dynamic_lookup)

# ---------- pg_jitter_mir ----------
add_library(pg_jitter_mir MODULE ${COMMON_SRC} src/pg_jitter_mir.c)
target_include_directories(pg_jitter_mir PRIVATE ${PG_INCLUDEDIR_SERVER} src)
target_link_libraries(pg_jitter_mir PRIVATE mir_lib)
set_target_properties(pg_jitter_mir PROPERTIES
    PREFIX ""
    SUFFIX ".dylib"
    C_VISIBILITY_PRESET default
)
target_link_options(pg_jitter_mir PRIVATE -bundle -undefined dynamic_lookup)

# ---------- Install ----------
install(TARGETS pg_jitter_sljit pg_jitter_asmjit pg_jitter_mir
    LIBRARY DESTINATION ${PG_PKGLIBDIR})
