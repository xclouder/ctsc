cmake_minimum_required(VERSION 3.20)
project(ctsc
    VERSION 0.0.1
    LANGUAGES C
    DESCRIPTION "C port of tsc (microsoft/TypeScript). Driven by the ctsc harness."
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /wd4200 /wd4706)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

add_library(ctsc_core STATIC
    src/core/arena.c
    src/core/string_ref.c
    src/core/hashmap.c
    src/core/diagnostic.c
    src/core/utf8.c
    src/core/json_writer.c
    src/core/buffer.c
)
target_include_directories(ctsc_core PUBLIC include)

add_library(ctsc_scanner STATIC
    src/scanner/scanner.c
    src/scanner/token_names.c
)
target_link_libraries(ctsc_scanner PUBLIC ctsc_core)

add_library(ctsc_parser STATIC
    src/parser/ast.c
    src/parser/parser.c
    src/parser/ast_json.c
)
target_link_libraries(ctsc_parser PUBLIC ctsc_scanner ctsc_core)

add_library(ctsc_binder STATIC
    src/binder/binder.c
    src/binder/symbol.c
)
target_link_libraries(ctsc_binder PUBLIC ctsc_parser ctsc_core)

add_library(ctsc_emitter STATIC
    src/emitter/emitter.c
)
target_link_libraries(ctsc_emitter PUBLIC ctsc_parser ctsc_core)

add_executable(ctsc
    src/driver/main.c
)
target_link_libraries(ctsc PRIVATE ctsc_emitter ctsc_binder ctsc_parser ctsc_scanner ctsc_core)

enable_testing()
add_executable(ctsc_tests
    tests/test_main.c
    tests/test_arena.c
    tests/test_string_ref.c
    tests/test_scanner.c
    tests/test_parser.c
    tests/test_binder.c
)
target_link_libraries(ctsc_tests PRIVATE ctsc_emitter ctsc_binder ctsc_parser ctsc_scanner ctsc_core)
add_test(NAME ctsc_tests COMMAND ctsc_tests)
