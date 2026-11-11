cmake_minimum_required(VERSION 3.20)

project(DyslexiaReaderDesktop LANGUAGES CXX)

add_library(dyslexia_reader_core src/caching_cloud_speech_client.cpp)
target_include_directories(dyslexia_reader_core PUBLIC include)
target_compile_features(dyslexia_reader_core PUBLIC cxx_std_17)

add_executable(contracts_smoke tests/contracts_smoke.cpp)
target_include_directories(contracts_smoke PRIVATE include)
target_compile_features(contracts_smoke PRIVATE cxx_std_17)

add_executable(cloud_speech_cache_smoke tests/cloud_speech_cache_smoke.cpp)
target_link_libraries(cloud_speech_cache_smoke PRIVATE dyslexia_reader_core)

enable_testing()
add_test(NAME contracts_smoke COMMAND contracts_smoke)
add_test(NAME cloud_speech_cache_smoke COMMAND cloud_speech_cache_smoke)

if(WIN32)
    enable_language(RC)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/docs/contracts/color-themes.json"
         theme_json)
    foreach(theme_name IN ITEMS warm-cream sepia dark-high-contrast)
        string(TOUPPER "${theme_name}" theme_variable)
        string(REPLACE "-" "_" theme_variable "${theme_variable}")
        foreach(token IN ITEMS background text surface border focus
                               sentenceHighlight wordHighlightBorder
                               rulerHighlight)
            string(TOUPPER "${token}" token_variable)
            string(JSON palette_value GET "${theme_json}" "${theme_name}"
                   "${token}")
            set("${theme_variable}_${token_variable}" "${palette_value}")
        endforeach()
    endforeach()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    configure_file(
        resources/theme_palette.hpp.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/theme_palette.hpp"
        @ONLY
    )
    configure_file(
        resources/fonts.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/fonts.rc"
        @ONLY
    )

    add_library(dyslexia_reader_windows
        src/windows/direct2d_text_renderer.cpp
        src/windows/font_resource_loader.cpp
        src/windows/in_memory_font_collection.cpp
        src/windows/reading_aids.cpp
        src/windows/speech_synthesizer.cpp
        src/windows/cloud_speech_client.cpp
        src/windows/audio_cache.cpp
    )
    target_include_directories(dyslexia_reader_windows
        PUBLIC include
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
    target_compile_features(dyslexia_reader_windows PUBLIC cxx_std_17)
    target_compile_definitions(dyslexia_reader_windows
        PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
               WINVER=0x0A00 _WIN32_WINNT=0x0A00
    )
    target_link_libraries(dyslexia_reader_windows
        PUBLIC dyslexia_reader_core bcrypt comctl32 crypt32 d2d1 dwrite
               user32 windowsapp winhttp
    )
    target_compile_options(dyslexia_reader_windows PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )

    add_executable(dyslexia_reader_demo WIN32
        examples/windows_reader_demo.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc
    )
    target_link_libraries(dyslexia_reader_demo PRIVATE dyslexia_reader_windows)

    add_executable(windows_reading_aids_smoke tests/windows_reading_aids.cpp)
    target_link_libraries(windows_reading_aids_smoke
        PRIVATE dyslexia_reader_windows
    )
    add_test(NAME windows_reading_aids_smoke COMMAND windows_reading_aids_smoke)
endif()
