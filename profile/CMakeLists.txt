set(PSPWEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(PSPWEB_GAME "hello" CACHE STRING
    "Title whose generated AOT corpus is linked (profiles/web/generated/<game>)")
set(PSPWEB_GUEST_PATH "ms0:/PSP/GAME/PSPWEB/EBOOT.PBP" CACHE STRING
    "Module path passed to module_start as argp")
set(PSPWEB_LINK_OPT "1" CACHE STRING
    "Emscripten only: link-time optimization level (wasm-opt -O3 on the full corpus takes very long)")
option(PSPWEB_PROFILING "Emscripten only: keep function names in the wasm for browser profilers" OFF)
set(PSPWEB_PRELOAD_DIR "" CACHE PATH
    "Emscripten only: host directory preloaded as /game (EBOOT.BIN, disc/, ms0/)")

set(PSPWEB_GENERATED_DIR "${PSPWEB_DIR}/generated/${PSPWEB_GAME}")
# Recursive: modules the game loads at run time are translated into modules/<name>.
file(GLOB_RECURSE PSPWEB_GENERATED CONFIGURE_DEPENDS "${PSPWEB_GENERATED_DIR}/*.cpp")
if(NOT PSPWEB_GENERATED)
    message(FATAL_ERROR "No generated AOT code in ${PSPWEB_GENERATED_DIR}; run scripts/generate.sh first")
endif()

# Generated units are huge single functions; keep them at the configured
# optimization level and silence warnings that are expected in emitted code.
add_library(pspweb_generated STATIC ${PSPWEB_GENERATED})
target_link_libraries(pspweb_generated PUBLIC psprecomp_core)
target_compile_options(pspweb_generated PRIVATE -O${PSPRECOMP_GENERATED_OPT_LEVEL} -w)

# ATRAC3/ATRAC3+ decoder (FFmpeg code via PPSSPP, LGPL 2.1+), used for streamed music and speech.
set(PSPWEB_AT3_DIR "${PSPWEB_DIR}/third_party/at3_standalone")
add_library(pspweb_at3 STATIC
    ${PSPWEB_AT3_DIR}/atrac.cpp ${PSPWEB_AT3_DIR}/atrac3.cpp ${PSPWEB_AT3_DIR}/atrac3plus.cpp
    ${PSPWEB_AT3_DIR}/atrac3plusdec.cpp ${PSPWEB_AT3_DIR}/atrac3plusdsp.cpp ${PSPWEB_AT3_DIR}/get_bits.cpp
    ${PSPWEB_AT3_DIR}/compat.cpp ${PSPWEB_AT3_DIR}/fft.cpp ${PSPWEB_AT3_DIR}/mem.cpp)
target_include_directories(pspweb_at3 PRIVATE "${PSPWEB_DIR}/third_party/at3_shim")
target_include_directories(pspweb_at3 PUBLIC "${PSPWEB_AT3_DIR}")
target_compile_options(pspweb_at3 PRIVATE -O2 -w)

add_executable(pspweb host/main.cpp host/kernel.cpp host/devices.cpp host/io.cpp host/modules.cpp host/pgd.cpp host/mpeg.cpp host/sas.cpp host/atrac.cpp host/savedata.cpp host/ge.cpp host/ge_gl.cpp host/ge_worker.cpp host/webfs.cpp)
# The table of translated run-time modules, or an empty one.
if(NOT EXISTS "${PSPWEB_GENERATED_DIR}/modules_table.cpp")
    target_sources(pspweb PRIVATE host/no_modules.cpp)
endif()
target_link_libraries(pspweb PRIVATE pspweb_generated psprecomp_core pspweb_at3)
target_compile_definitions(pspweb PRIVATE PSPWEB_GUEST_PATH="${PSPWEB_GUEST_PATH}")
target_compile_options(pspweb PRIVATE -Wall -Wextra)
set_target_properties(pspweb PROPERTIES OUTPUT_NAME "pspweb_${PSPWEB_GAME}")

if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(pspweb PRIVATE EGL GLESv2 Threads::Threads)
endif()

if(EMSCRIPTEN)
    if(PSPWEB_PRELOAD_DIR STREQUAL "")
        message(FATAL_ERROR "PSPWEB_PRELOAD_DIR is required for the Emscripten build")
    endif()
    set_target_properties(pspweb PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")
    # 128-bit SIMD lets the compiler vectorize the GE's vertex math (all current browsers support it).
    target_compile_options(pspweb PRIVATE -msimd128)
    target_compile_options(pspweb_at3 PRIVATE -msimd128)
    target_link_options(pspweb PRIVATE
        -O${PSPWEB_LINK_OPT}
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=134217728
        -sSTACK_SIZE=8388608
        -fwasm-exceptions
        # The GE runs on a thread of its own with the canvas (OffscreenCanvas);
        # the page must be served cross-origin isolated (see scripts/serve.py).
        -pthread
        -sPTHREAD_POOL_SIZE=2
        -sDEFAULT_PTHREAD_STACK_SIZE=2097152
        -sOFFSCREENCANVAS_SUPPORT=1
        -lidbfs.js
        "-sEXPORTED_FUNCTIONS=_main,_malloc,_free"
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        "--preload-file=${PSPWEB_PRELOAD_DIR}@/game"
        "--shell-file=${PSPWEB_DIR}/web/shell.html")
    if(PSPWEB_PROFILING)
        target_link_options(pspweb PRIVATE --profiling-funcs)
    endif()
    set_property(TARGET pspweb APPEND PROPERTY LINK_DEPENDS "${PSPWEB_DIR}/web/shell.html")
endif()
