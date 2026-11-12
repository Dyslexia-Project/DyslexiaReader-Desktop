# ==============================================================================
# 0. Suport vcpkg pe Windows (trebuie plasat inainte de project())
# ==============================================================================
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _vcpkg_root)
    set(CMAKE_TOOLCHAIN_FILE "${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    unset(_vcpkg_root)
endif()

cmake_minimum_required(VERSION 3.20)

project(DyslexiaReaderDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DYSLEXIA_ENABLE_TESSERACT "Enable Tesseract OCR integration" ON)

# ==============================================================================
# 1. Dependinte Tesseract OCR & Leptonica (Cross-Platform)
# ==============================================================================
set(DYSLEXIA_OCR_INCLUDE_DIRS "")
set(DYSLEXIA_OCR_LINK_DIRS "")
set(DYSLEXIA_TESSERACT_TARGET "")
set(DYSLEXIA_LEPTONICA_TARGET "")

if(DYSLEXIA_ENABLE_TESSERACT)
    find_package(PkgConfig QUIET)

    if(PkgConfig_FOUND)
        pkg_check_modules(TESSERACT QUIET IMPORTED_TARGET tesseract)
        pkg_check_modules(LEPTONICA QUIET IMPORTED_TARGET lept)
    endif()

    if(TARGET PkgConfig::TESSERACT)
        set(DYSLEXIA_TESSERACT_TARGET PkgConfig::TESSERACT)
    else()
        find_package(Tesseract CONFIG QUIET)
        if(NOT Tesseract_FOUND)
            find_package(Tesseract REQUIRED)
        endif()
        if(TARGET Tesseract::libtesseract)
            set(DYSLEXIA_TESSERACT_TARGET Tesseract::libtesseract)
        elseif(TARGET Tesseract::tesseract)
            set(DYSLEXIA_TESSERACT_TARGET Tesseract::tesseract)
        else()
            message(FATAL_ERROR "Tesseract package found, but no CMake target was exported.")
        endif()
    endif()

    if(TARGET PkgConfig::LEPTONICA)
        set(DYSLEXIA_LEPTONICA_TARGET PkgConfig::LEPTONICA)
    else()
        find_package(Leptonica CONFIG QUIET)
        if(NOT Leptonica_FOUND)
            find_package(Leptonica REQUIRED)
        endif()
        if(TARGET Leptonica::leptonica)
            set(DYSLEXIA_LEPTONICA_TARGET Leptonica::leptonica)
        elseif(TARGET Leptonica::Leptonica)
            set(DYSLEXIA_LEPTONICA_TARGET Leptonica::Leptonica)
        elseif(TARGET leptonica)
            set(DYSLEXIA_LEPTONICA_TARGET leptonica)
        elseif(DEFINED LEPTONICA_LIBRARIES)
            set(DYSLEXIA_LEPTONICA_TARGET ${LEPTONICA_LIBRARIES})
        endif()
    endif()

    if(APPLE)
        if(EXISTS "/opt/homebrew/include")
            list(APPEND DYSLEXIA_OCR_INCLUDE_DIRS /opt/homebrew/include)
        endif()
        if(EXISTS "/opt/homebrew/include/leptonica")
            list(APPEND DYSLEXIA_OCR_INCLUDE_DIRS /opt/homebrew/include/leptonica)
        endif()
    endif()

    if(TESSERACT_LIBRARY_DIRS)
        list(APPEND DYSLEXIA_OCR_LINK_DIRS ${TESSERACT_LIBRARY_DIRS})
    endif()
    if(LEPTONICA_LIBRARY_DIRS)
        list(APPEND DYSLEXIA_OCR_LINK_DIRS ${LEPTONICA_LIBRARY_DIRS})
    endif()
endif()

# ==============================================================================
# 2. Descarcare automata a modelului OCR (eng.traineddata)
# ==============================================================================
if(DYSLEXIA_ENABLE_TESSERACT)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tessdata")
    if(NOT EXISTS "${CMAKE_BINARY_DIR}/tessdata/eng.traineddata")
        message(STATUS "[OCR] Se descarca modelul de limba engleza (eng.traineddata)...")
        file(DOWNLOAD
            "https://github.com/tesseract-ocr/tessdata_fast/raw/main/eng.traineddata"
            "${CMAKE_BINARY_DIR}/tessdata/eng.traineddata"
            SHOW_PROGRESS
            STATUS _traineddata_status
        )
        list(GET _traineddata_status 0 _traineddata_code)
        if(NOT _traineddata_code EQUAL 0)
            file(REMOVE "${CMAKE_BINARY_DIR}/tessdata/eng.traineddata")
            list(GET _traineddata_status 1 _traineddata_error)
            message(WARNING "[OCR] Descarcarea eng.traineddata a esuat (${_traineddata_error}).")
        endif()
    endif()
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(resources/ocr_build_paths.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ocr_build_paths.hpp" @ONLY)

# ==============================================================================
# 3. Modulele Core si OCR
# ==============================================================================
add_library(dyslexia_reader_core
    src/caching_cloud_speech_client.cpp
    src/paragraph_rebuilder.cpp
)
target_include_directories(dyslexia_reader_core PUBLIC include)
target_compile_features(dyslexia_reader_core PUBLIC cxx_std_17)

if(WIN32)
    set(OCR_SRC src/windows/ocr_service.cpp)
else()
    set(OCR_SRC src/mac/ocr_service.cpp)
endif()
if(DYSLEXIA_ENABLE_TESSERACT)
    list(APPEND OCR_SRC src/tesseract_ocr.cpp)
endif()
list(APPEND OCR_SRC src/tesseract_ocr_engine.cpp)

add_library(dyslexia_reader_ocr ${OCR_SRC})
target_include_directories(dyslexia_reader_ocr
    PUBLIC include
    PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
if(DYSLEXIA_OCR_INCLUDE_DIRS)
    target_include_directories(dyslexia_reader_ocr PRIVATE ${DYSLEXIA_OCR_INCLUDE_DIRS})
endif()
if(DYSLEXIA_OCR_LINK_DIRS)
    target_link_directories(dyslexia_reader_ocr PRIVATE ${DYSLEXIA_OCR_LINK_DIRS})
endif()
target_compile_features(dyslexia_reader_ocr PUBLIC cxx_std_17)
target_link_libraries(dyslexia_reader_ocr PUBLIC dyslexia_reader_core)

if(DYSLEXIA_ENABLE_TESSERACT)
    target_link_libraries(dyslexia_reader_ocr PRIVATE ${DYSLEXIA_TESSERACT_TARGET} ${DYSLEXIA_LEPTONICA_TARGET})
    target_compile_definitions(dyslexia_reader_ocr PUBLIC DYSLEXIA_HAS_TESSERACT=1)
endif()

# ==============================================================================
# 4. Teste si Executabile Smoke
# ==============================================================================
add_executable(contracts_smoke tests/contracts_smoke.cpp)
target_include_directories(contracts_smoke PRIVATE include)
target_compile_features(contracts_smoke PRIVATE cxx_std_17)

add_executable(cloud_speech_cache_smoke tests/cloud_speech_cache_smoke.cpp)
target_link_libraries(cloud_speech_cache_smoke PRIVATE dyslexia_reader_core)

enable_testing()
add_test(NAME contracts_smoke COMMAND contracts_smoke)
add_test(NAME cloud_speech_cache_smoke COMMAND cloud_speech_cache_smoke)

add_executable(paragraph_rebuilder_smoke tests/paragraph_rebuilder_smoke.cpp)
target_link_libraries(paragraph_rebuilder_smoke PRIVATE dyslexia_reader_ocr)
target_include_directories(paragraph_rebuilder_smoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_test(NAME paragraph_rebuilder_smoke COMMAND paragraph_rebuilder_smoke)

# ==============================================================================
# 5. Target Demo Tesseract pe macOS / Non-Windows si CLI OCR pe Windows
# ==============================================================================
if(DYSLEXIA_ENABLE_TESSERACT)
    if(WIN32)
        add_executable(ocr_cli_demo src/main.cpp)
        target_compile_features(ocr_cli_demo PRIVATE cxx_std_17)
        target_include_directories(ocr_cli_demo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(ocr_cli_demo PRIVATE dyslexia_reader_ocr)
    else()
        add_executable(dyslexia_reader_demo src/main.cpp)
        target_compile_features(dyslexia_reader_demo PRIVATE cxx_std_17)
        target_include_directories(dyslexia_reader_demo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(dyslexia_reader_demo PRIVATE dyslexia_reader_ocr)
    endif()
endif()

# ==============================================================================
# 6. Target-uri specifice pentru Windows (Visual Studio / Direct2D)
# ==============================================================================
if(WIN32)
    enable_language(RC)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/docs/contracts/color-themes.json" theme_json)
    foreach(theme_name IN ITEMS warm-cream sepia dark-high-contrast)
        string(TOUPPER "${theme_name}" theme_variable)
        string(REPLACE "-" "_" theme_variable "${theme_variable}")
        foreach(token IN ITEMS background text surface border focus sentenceHighlight wordHighlightBorder rulerHighlight)
            string(TOUPPER "${token}" token_variable)
            string(JSON palette_value GET "${theme_json}" "${theme_name}" "${token}")
            set("${theme_variable}_${token_variable}" "${palette_value}")
        endforeach()
    endforeach()

    configure_file(resources/theme_palette.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/theme_palette.hpp" @ONLY)
    configure_file(resources/fonts.rc.in "${CMAKE_CURRENT_BINARY_DIR}/fonts.rc" @ONLY)

    add_library(dyslexia_reader_windows
        src/windows/direct2d_text_renderer.cpp
        src/windows/font_resource_loader.cpp
        src/windows/in_memory_font_collection.cpp
        src/windows/reading_aids.cpp
        src/windows/speech_synthesizer.cpp
        src/windows/cloud_speech_client.cpp
        src/windows/audio_cache.cpp
    )
    target_include_directories(dyslexia_reader_windows PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_features(dyslexia_reader_windows PUBLIC cxx_std_17)
    target_compile_definitions(dyslexia_reader_windows PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    find_package(nlohmann_json CONFIG REQUIRED)
    target_sources(dyslexia_reader_windows PRIVATE
        src/windows/profile_store.cpp
        src/windows/screen_capturer.cpp
        src/windows/tray_shell.cpp
    )
    target_link_libraries(dyslexia_reader_windows
        PUBLIC dyslexia_reader_core bcrypt comctl32 crypt32 d2d1 dwrite gdi32 ole32 shell32 user32 windowsapp winhttp
        PRIVATE nlohmann_json::nlohmann_json
    )
    target_compile_options(dyslexia_reader_windows PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4> $<$<CXX_COMPILER_ID:MSVC>:/permissive-> $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

    add_executable(dyslexia_reader_demo WIN32 examples/windows_reader_demo.cpp ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc)
    target_compile_features(dyslexia_reader_demo PRIVATE cxx_std_17)
    target_link_libraries(dyslexia_reader_demo PRIVATE dyslexia_reader_windows dyslexia_reader_ocr)

    add_executable(windows_reading_aids_smoke tests/windows_reading_aids.cpp)
    target_link_libraries(windows_reading_aids_smoke PRIVATE dyslexia_reader_windows)
    add_test(NAME windows_reading_aids_smoke COMMAND windows_reading_aids_smoke)

    add_executable(profile_store_smoke tests/profile_store_smoke.cpp)
    target_link_libraries(profile_store_smoke PRIVATE dyslexia_reader_windows)
    add_test(NAME profile_store_smoke COMMAND profile_store_smoke)

    add_executable(windows_renderer_smoke tests/windows_renderer_smoke.cpp ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc)
    target_link_libraries(windows_renderer_smoke PRIVATE dyslexia_reader_windows)
    add_test(NAME windows_renderer_smoke COMMAND windows_renderer_smoke)
endif()
