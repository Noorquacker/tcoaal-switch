# Host (Linux) build of the runtime, used for development and for the jsbc
# bytecode compiler. The Switch build uses Makefile.switch.
cmake_minimum_required(VERSION 3.16)
project(tcoaal_runtime C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(qjs STATIC
  ${TP}/quickjs/dtoa.c ${TP}/quickjs/libregexp.c ${TP}/quickjs/libunicode.c ${TP}/quickjs/quickjs.c)
target_include_directories(qjs PUBLIC ${TP}/quickjs)
target_compile_definitions(qjs PRIVATE _GNU_SOURCE QUICKJS_NG_BUILD)
target_compile_options(qjs PRIVATE -w)
target_link_libraries(qjs PUBLIC m pthread)

file(GLOB PLUTOVG_SRC ${TP}/plutovg/source/*.c)
add_library(plutovg STATIC ${PLUTOVG_SRC})
target_include_directories(plutovg PUBLIC ${TP}/plutovg/include)
target_compile_definitions(plutovg PUBLIC PLUTOVG_BUILD_STATIC)
target_compile_options(plutovg PRIVATE -w)
target_link_libraries(plutovg PUBLIC m)

add_executable(jsbc src/jsbc.c)
target_link_libraries(jsbc qjs)

find_package(SDL2 REQUIRED)
find_package(ZLIB REQUIRED)
find_library(GLESV2 GLESv2 REQUIRED)

add_executable(tcoaal
  src/main.c src/sys.c src/canvas.c src/gl.c src/audio.c src/input.c)
target_include_directories(tcoaal PRIVATE src ${TP}/stb)
target_compile_options(tcoaal PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(src/audio.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-sign-compare;-Wno-unused-variable;-Wno-unused-value")
target_link_libraries(tcoaal qjs plutovg SDL2::SDL2 ZLIB::ZLIB ${GLESV2} m)
