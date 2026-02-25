set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---------- Paths ----------
set(PG_CONFIG "pg_config" CACHE FILEPATH "Path to pg_config")
set(ASMJIT_DIR "${ROOT}/../asmjit" CACHE PATH "Path to asmjit source directory")

execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS "PG server includes: ${PG_INCLUDEDIR_SERVER}")
message(STATUS "PG pkglibdir:       ${PG_PKGLIBDIR}")
message(STATUS "asmjit source:      ${ASMJIT_DIR}")

# ---------- AsmJIT (static library) ----------
set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${ASMJIT_DIR} ${CMAKE_BINARY_DIR}/asmjit)

# ---------- Pre-compiled deform templates ----------
set(DEFORM_GEN_SCRIPT "${ROOT}/tools/gen_deform_templates.py")
set(DEFORM_TEMPLATE_C "${ROOT}/src/pg_jit_deform_templates.c")

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_command(
        OUTPUT ${DEFORM_TEMPLATE_C}
        COMMAND ${Python3_EXECUTABLE} ${DEFORM_GEN_SCRIPT}
                --output-c ${DEFORM_TEMPLATE_C}
        DEPENDS ${DEFORM_GEN_SCRIPT}
        COMMENT "Generating pre-compiled deform templates"
    )
endif()

# ---------- Pre-compilation infrastructure (LLVM / c2mir) ----------
set(CMAKE_SOURCE_DIR "${ROOT}")
include(${ROOT}/cmake/precompiled.cmake)

# ---------- pg_jitter_asmjit ----------
set(COMMON_SRC ${ROOT}/src/pg_jitter_common.c ${ROOT}/src/pg_jit_funcs.c
    ${ROOT}/src/pg_jit_tier2_wrappers.c)

add_library(pg_jitter_asmjit MODULE ${COMMON_SRC}
            ${ROOT}/src/pg_jitter_asmjit.cpp
            ${ROOT}/src/pg_jit_deform_templates.c)
target_include_directories(pg_jitter_asmjit PRIVATE ${PG_INCLUDEDIR_SERVER} ${ROOT}/src)
target_link_libraries(pg_jitter_asmjit PRIVATE asmjit::asmjit)
pg_jitter_add_precompiled(pg_jitter_asmjit)

set_target_properties(pg_jitter_asmjit PROPERTIES PREFIX "" SUFFIX "${PG_MODULE_SUFFIX}"
    CXX_VISIBILITY_PRESET default C_VISIBILITY_PRESET default)
if(PG_MODULE_LINK_OPTIONS)
    target_link_options(pg_jitter_asmjit PRIVATE ${PG_MODULE_LINK_OPTIONS})
endif()

install(TARGETS pg_jitter_asmjit LIBRARY DESTINATION ${PG_PKGLIBDIR})
