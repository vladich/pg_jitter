set(CMAKE_C_STANDARD 11)

# ---------- Paths ----------
set(PG_CONFIG "pg_config" CACHE FILEPATH "Path to pg_config")
set(MIR_DIR "${ROOT}/../mir" CACHE PATH "Path to MIR source directory")
set(SLJIT_DIR "${ROOT}/../sljit" CACHE PATH "Path to sljit source directory (for deform JIT)")

execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS "PG server includes: ${PG_INCLUDEDIR_SERVER}")
message(STATUS "PG pkglibdir:       ${PG_PKGLIBDIR}")
message(STATUS "MIR source:         ${MIR_DIR}")

# ---------- MIR (static library) ----------
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_library(mir_lib STATIC ${MIR_DIR}/mir.c ${MIR_DIR}/mir-gen.c)
target_include_directories(mir_lib PUBLIC ${MIR_DIR})
target_compile_options(mir_lib PRIVATE -std=gnu11 -w)

# ---------- sljit (static library, for deform JIT shared across backends) ----------
add_library(sljit STATIC ${SLJIT_DIR}/sljit_src/sljitLir.c)
target_include_directories(sljit PUBLIC ${SLJIT_DIR}/sljit_src)
target_compile_options(sljit PRIVATE -w)

find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(mir_lib PRIVATE Threads::Threads)
    target_compile_definitions(mir_lib PRIVATE MIR_PARALLEL_GEN)
endif()

# ---------- Pre-compilation infrastructure (LLVM / c2mir) ----------
set(CMAKE_SOURCE_DIR "${ROOT}")
include(${ROOT}/cmake/precompiled.cmake)

# ---------- pg_jitter_mir ----------
set(COMMON_SRC ${ROOT}/src/pg_jitter_common.c ${ROOT}/src/pg_jit_funcs.c
    ${ROOT}/src/pg_jit_tier2_wrappers.c)

add_library(pg_jitter_mir MODULE ${COMMON_SRC}
            ${ROOT}/src/pg_jitter_mir.c
            ${ROOT}/src/pg_jitter_deform_jit.c)
target_include_directories(pg_jitter_mir PRIVATE ${PG_INCLUDEDIR_SERVER} ${ROOT}/src
                           ${SLJIT_DIR}/sljit_src)
target_link_libraries(pg_jitter_mir PRIVATE mir_lib sljit)
pg_jitter_add_precompiled(pg_jitter_mir)

set_target_properties(pg_jitter_mir PROPERTIES PREFIX "" SUFFIX "${PG_MODULE_SUFFIX}"
    C_VISIBILITY_PRESET default)
if(PG_MODULE_LINK_OPTIONS)
    target_link_options(pg_jitter_mir PRIVATE ${PG_MODULE_LINK_OPTIONS})
endif()

install(TARGETS pg_jitter_mir LIBRARY DESTINATION ${PG_PKGLIBDIR})
