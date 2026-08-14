cmake_minimum_required(VERSION 3.21)

project(PXMLCompiler VERSION 0.1.0 LANGUAGES C)

option(PXML_BUILD_TESTS "Build the PXML compiler test suite" ON)
option(PXML_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(pxml_configure_target target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(PXML_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        if(PXML_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(pxml_core STATIC
    src/common.c
    src/arena.c
    src/simd/scan.c
    src/syntax.c
    src/lexer.c
    src/parser.c)
pxml_configure_target(pxml_core)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" PXML_SYSTEM_PROCESSOR)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" AND
   PXML_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    target_sources(pxml_core PRIVATE src/simd/scan_avx2.c)
    set_source_files_properties(src/simd/scan_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2")
    target_compile_definitions(pxml_core PRIVATE PXML_HAVE_AVX2_SCANNER=1)
elseif(PXML_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_sources(pxml_core PRIVATE src/simd/scan_neon.c)
    target_compile_definitions(pxml_core PRIVATE PXML_HAVE_NEON_SCANNER=1)
endif()

add_library(pxml_expand STATIC src/expander.c)
target_link_libraries(pxml_expand PUBLIC pxml_core)
pxml_configure_target(pxml_expand)

add_library(pxml_opt STATIC
    src/ir.c
    src/optimizer.c)
target_link_libraries(pxml_opt PUBLIC pxml_core)
pxml_configure_target(pxml_opt)

add_library(pxml_compile STATIC
    src/expression.c
    src/compiler.c
    src/binary.c)
target_link_libraries(pxml_compile PUBLIC pxml_opt)
pxml_configure_target(pxml_compile)

add_library(pxml_full STATIC src/full.c)
target_link_libraries(pxml_full PUBLIC pxml_expand pxml_compile)
pxml_configure_target(pxml_full)

add_library(pxml_compiler ALIAS pxml_full)

add_executable(pxml-expand src/expand_main.c src/cli_support.c)
target_link_libraries(pxml-expand PRIVATE pxml_expand)
pxml_configure_target(pxml-expand)

add_executable(pxml-opt src/opt_main.c src/cli_support.c)
target_link_libraries(pxml-opt PRIVATE pxml_opt)
pxml_configure_target(pxml-opt)

add_executable(pxml-compile src/compile_main.c src/cli_support.c)
target_link_libraries(pxml-compile PRIVATE pxml_compile)
pxml_configure_target(pxml-compile)

add_executable(pxmlc src/main.c)
target_link_libraries(pxmlc PRIVATE pxml_full)
pxml_configure_target(pxmlc)

include(GNUInstallDirs)
install(TARGETS
    pxml-expand pxml-opt pxml-compile pxmlc
    pxml_core pxml_expand pxml_opt pxml_compile pxml_full
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md LICENSE DESTINATION .)
install(DIRECTORY docs/ DESTINATION share/doc/pxml)

if(PXML_BUILD_TESTS)
    enable_testing()
    add_executable(pxml_tests tests/test_main.c)
    target_link_libraries(pxml_tests PRIVATE pxml_full)
    pxml_configure_target(pxml_tests)
    add_test(NAME pxml_tests COMMAND pxml_tests)
endif()
