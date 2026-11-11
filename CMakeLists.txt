# ==============================================================================
# 0. vcpkg support for Windows (must precede project())
# ==============================================================================
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

cmake_minimum_required(VERSION 3.20)

project(DyslexiaReaderDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ==============================================================================
# 1. Tesseract OCR & Leptonica Dependencies (Cross-Platform)
# ==============================================================================
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(TESSERACT tesseract)
    pkg_check_modules(LEPTONICA lept)
endif()

# Fallback for vcpkg or system packages when PkgConfig does not find them directly
if(NOT TESSERACT_FOUND)
    find_package(Tesseract REQUIRED)
endif()

if(NOT LEPTONICA_FOUND)
    find_package(Leptonica REQUIRED)
endif()

# Add library directories for macOS / Homebrew
if(TESSERACT_LIBRARY_DIRS OR LEPTONICA_LIBRARY_DIRS)
    link_directories(${TESSERACT_LIBRARY_DIRS} ${LEPTONICA_LIBRARY_DIRS})
endif()

# ==============================================================================
# 2. Automatic OCR model download (eng.traineddata)
# ==============================================================================
if(NOT EXISTS "${CMAKE_BINARY_DIR}/tessdata/eng.traineddata")
    message(STATUS "[OCR] Downloading English language model (eng.traineddata)...")
    file(DOWNLOAD 
        "https://github.com/tesseract-ocr/tessdata_fast/raw/main/eng.traineddata"
        "${CMAKE_BINARY_DIR}/tessdata/eng.traineddata"
        SHOW_PROGRESS
    )
endif()

# ==============================================================================
# 3. Core & OCR Modules
# ==============================================================================
add_library(dyslexia_reader_core src/caching_cloud_speech_client.cpp)
target_include_directories(dyslexia_reader_core PUBLIC include)
target_compile_features(dyslexia_reader_core PRIVATE cxx_std_17)

if(WIN32)
    set(OCR_SRC src/windows/ocr_service.cpp)
else()
    set(OCR_SRC src/mac/ocr_service.cpp)
endif()

add_library(dyslexia_reader_ocr ${OCR_SRC})
target_include_directories(dyslexia_reader_ocr 
    PUBLIC include include/dyslexia_reader
    PRIVATE ${TESSERACT_INCLUDE_DIRS} ${LEPTONICA_INCLUDE_DIRS} /opt/homebrew/include /opt/homebrew/include/leptonica
)
target_compile_features(dyslexia_reader_ocr PUBLIC cxx_std_17)

if(TESSERACT_LIBRARY_DIRS OR LEPTONICA_LIBRARY_DIRS)
    target_link_directories(dyslexia_reader_ocr PRIVATE ${TESSERACT_LIBRARY_DIRS} ${LEPTONICA_LIBRARY_DIRS})
endif()

if(TESSERACT_LIBRARIES)
    target_link_libraries(dyslexia_reader_ocr PRIVATE ${TESSERACT_LIBRARIES} ${LEPTONICA_LIBRARIES})
else()
    target_link_libraries(dyslexia_reader_ocr PRIVATE Tesseract::tesseract)
endif()

# ==============================================================================
# 4. Tests and Smoke Executables
# ==============================================================================
add_executable(contracts_smoke tests/contracts_smoke.cpp)
target_include_directories(contracts_smoke PRIVATE include)
target_compile_features(contracts_smoke PRIVATE cxx_std_17)

add_executable(cloud_speech_cache_smoke tests/cloud_speech_cache_smoke.cpp)
target_link_libraries(cloud_speech_cache_smoke PRIVATE dyslexia_reader_core)

enable_testing()
add_test(NAME contracts_smoke COMMAND contracts_smoke)
add_test(NAME cloud_speech_cache_smoke COMMAND cloud_speech_cache_smoke)

# ==============================================================================
# 5. Tesseract Demo Target for macOS / Non-Windows
# ==============================================================================
if(NOT WIN32)
    add_executable(dyslexia_reader_demo src/main.cpp)
    target_compile_features(dyslexia_reader_demo PRIVATE cxx_std_17)
    target_include_directories(dyslexia_reader_demo 
        PRIVATE include include/dyslexia_reader ${TESSERACT_INCLUDE_DIRS} ${LEPTONICA_INCLUDE_DIRS} /opt/homebrew/include /opt/homebrew/include/leptonica
    )
    if(TESSERACT_LIBRARY_DIRS OR LEPTONICA_LIBRARY_DIRS)
        target_link_directories(dyslexia_reader_demo PRIVATE ${TESSERACT_LIBRARY_DIRS} ${LEPTONICA_LIBRARY_DIRS})
    endif()
    
    target_link_libraries(dyslexia_reader_demo PRIVATE dyslexia_reader_ocr)
    if(TESSERACT_LIBRARIES)
        target_link_libraries(dyslexia_reader_demo PRIVATE ${TESSERACT_LIBRARIES} ${LEPTONICA_LIBRARIES})
    else()
        target_link_libraries(dyslexia_reader_demo PRIVATE Tesseract::tesseract)
    endif()
endif()

# ==============================================================================
# 6. Windows-specific targets (Visual Studio / Direct2D)
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
    
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
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
    target_link_libraries(dyslexia_reader_windows PUBLIC dyslexia_reader_core bcrypt comctl32 crypt32 d2d1 dwrite user32 windowsapp winhttp)
    target_compile_options(dyslexia_reader_windows PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4> $<$<CXX_COMPILER_ID:MSVC>:/permissive-> $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

    add_executable(dyslexia_reader_demo WIN32 examples/windows_reader_demo.cpp ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc)
    target_compile_features(dyslexia_reader_demo PRIVATE cxx_std_17)
    target_link_libraries(dyslexia_reader_demo PRIVATE dyslexia_reader_windows dyslexia_reader_ocr)

    add_executable(windows_reading_aids_smoke tests/windows_reading_aids.cpp)
    target_link_libraries(windows_reading_aids_smoke PRIVATE dyslexia_reader_windows)
    add_test(NAME windows_reading_aids_smoke COMMAND windows_reading_aids_smoke)
endif()