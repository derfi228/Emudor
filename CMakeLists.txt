cmake_minimum_required(VERSION 3.20)
project(NesEmu VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Режимы сборки
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Предупреждения как ошибки
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)
endif()

# Дефайны по режиму
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(NES_DEBUG)
endif()

# ─── Поиск зависимостей (MinGW/MSYS2) ───────────────────────────────────────
find_package(SDL2 REQUIRED)
find_package(SDL2_image REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)
# libcurl — загрузка обложек из Libretro thumbnails (опционально)
find_package(CURL)
if(CURL_FOUND)
    message(STATUS "libcurl found — cover download enabled")
else()
    message(STATUS "libcurl not found — cover download disabled")
endif()

# ─── ImGui (вендорный) ──────────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
)
target_compile_options(imgui PRIVATE -w)  # без предупреждений для вендорного кода

# ─── tinyfiledialogs (вендорный) ────────────────────────────────────────────
add_library(tinyfd STATIC ${CMAKE_SOURCE_DIR}/third_party/tinyfiledialogs.c)
target_include_directories(tinyfd PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_compile_options(tinyfd PRIVATE -w)

# ─── nes_core (статическая библиотека) ──────────────────────────────────────
add_library(nes_core STATIC
    src/cpu/cpu.cpp
    src/ppu/ppu.cpp
    src/memory/memory_bus.cpp
    src/apu/apu.cpp
    src/mapper/mapper1.cpp
    src/mapper/mapper4.cpp
    src/mapper/mapper69.cpp
    src/console/nes_console.cpp
    src/console/snes_console.cpp
    src/snes/cpu65816.cpp
    src/snes/snes_bus.cpp
    src/snes/snes_ppu.cpp
    src/snes/snes_apu.cpp
    src/snes/superfx.cpp
    src/snes/snes_dsp1.cpp
)
target_include_directories(nes_core PUBLIC src)
target_compile_features(nes_core PUBLIC cxx_std_17)

# ─── emudor (основной exe) ─────────────────────────────────────────────────
add_executable(emudor
    src/main.cpp
    src/app.cpp
    src/cli.cpp
)

if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(emudor PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_link_libraries(emudor PRIVATE
    nes_core
    imgui
    tinyfd
    SDL2::SDL2main   # WinMain stub для MinGW, должен идти до SDL2::SDL2
    SDL2::SDL2
    SDL2_image::SDL2_image
    nlohmann_json::nlohmann_json
    -lcomdlg32 -lole32  # нужны tinyfiledialogs на Windows
)
if(CURL_FOUND)
    target_link_libraries(emudor PRIVATE CURL::libcurl)
    target_compile_definitions(emudor PRIVATE NES_HAVE_CURL)
endif()

# Копировать DLL рядом с exe
if(WIN32)
    add_custom_command(TARGET emudor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:emudor>
        COMMENT "Copying SDL2 DLL"
    )
    # SDL2_image DLL
    get_target_property(_sdl2img_loc SDL2_image::SDL2_image LOCATION)
    if(_sdl2img_loc)
        add_custom_command(TARGET emudor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_sdl2img_loc}
                $<TARGET_FILE_DIR:emudor>
            COMMENT "Copying SDL2_image DLL"
        )
    endif()
endif()

# ─── nes_tests ───────────────────────────────────────────────────────────────
add_executable(nes_tests
    tests/cpu_test.cpp
    tests/ppu_test.cpp
    tests/memory_test.cpp
    tests/snes_cpu_test.cpp
    tests/snes_bus_test.cpp
)
target_link_libraries(nes_tests PRIVATE
    nes_core
    GTest::GTest
    GTest::Main
)
# Копируем DLL GTest рядом с тест-бинарником (нужно на Windows с shared GTest)
add_custom_command(TARGET nes_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GTest::gtest>
        $<TARGET_FILE:GTest::gtest_main>
        $<TARGET_FILE_DIR:nes_tests>
    COMMENT "Copying GTest DLLs"
)
enable_testing()
add_test(NAME NesTests COMMAND nes_tests)

# ─── nestest (инструмент) ────────────────────────────────────────────────────
add_executable(nestest
    tools/nestest.cpp
)
target_link_libraries(nestest PRIVATE nes_core)

# ─── batman_trace (инструмент) ───────────────────────────────────────────────
add_executable(batman_trace
    tools/batman_trace.cpp
)
target_link_libraries(batman_trace PRIVATE nes_core)
