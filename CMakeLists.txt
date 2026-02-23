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

# ---------- Common source ----------
set(COMMON_SRC src/pg_jitter_common.c src/pg_jit_funcs.c)

# ---------- pg_jitter_sljit ----------
add_library(pg_jitter_sljit MODULE ${COMMON_SRC} src/pg_jitter_sljit.c)
target_include_directories(pg_jitter_sljit PRIVATE ${PG_INCLUDEDIR_SERVER} src)
target_link_libraries(pg_jitter_sljit PRIVATE sljit)
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
