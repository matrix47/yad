cmake_minimum_required(VERSION 3.20)

if(DEFINED ESP_PLATFORM)
    # ESP-IDF component registration (uses pre-generated static assets)
    idf_component_register(
        SRCS
            src/core/config.cpp
            src/core/fb.cpp
            src/core/view.cpp
            src/core/net_proto.cpp
            src/core/providers.cpp
            src/core/engine.cpp
            src/core/generated_assets.cpp
            src/platforms/esp32/esp32_platform.cpp
        INCLUDE_DIRS
            include
            src/platforms/esp32
        REQUIRES
            json
            esp-tls
            lwip
            esp_timer
    )
    return()
endif()

project(yad VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(YAD_BUILD_POSIX "Build POSIX/mbedtls3 platform backend" ON)
option(YAD_BUILD_PREVIEW "Build SDL3 zero-Qt yad_preview application" ON)
set(YAD_REGEN_ASSETS "AUTO" CACHE STRING "Regenerate C++ asset tables via tools/gen_assets.py (AUTO, ON, OFF)")

find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)

set(YAD_ASSETS_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/core/generated_assets.cpp")
set(_yad_do_regen OFF)
if(YAD_REGEN_ASSETS STREQUAL "ON")
    set(_yad_do_regen ON)
elseif(YAD_REGEN_ASSETS STREQUAL "AUTO")
    find_program(_YAD_RSVG rsvg-convert)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(_YAD_RSVG AND Python3_Interpreter_FOUND)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import PIL"
            RESULT_VARIABLE _YAD_PIL_RC
            OUTPUT_QUIET ERROR_QUIET
        )
        if(_YAD_PIL_RC EQUAL 0 AND (EXISTS "/usr/share/fonts/misc/ter-u14n.otb" OR EXISTS "/usr/share/fonts/TTF/TerminusTTF.ttf"))
            set(_yad_do_regen ON)
        endif()
    endif()
endif()

if(_yad_do_regen)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB_RECURSE YAD_ASSET_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/frames/*.9.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/*.svg"
    )
    set(YAD_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated_assets.cpp")
    add_custom_command(
        OUTPUT "${YAD_ASSETS_CPP}"
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_assets.py"
            --assets-dir "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            --out "${YAD_ASSETS_CPP}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_assets.py"
            ${YAD_ASSET_INPUTS}
        COMMENT "Generating static C++ bitmap assets for libyad"
        VERBATIM
    )
else()
    message(STATUS "yad: using pre-generated src/core/generated_assets.cpp")
endif()

# 1. Portable core library (zero Qt, zero OS/GUI deps)
add_library(yad STATIC
    src/core/config.cpp
    src/core/fb.cpp
    src/core/view.cpp
    src/core/net_proto.cpp
    src/core/providers.cpp
    src/core/engine.cpp
    "${YAD_ASSETS_CPP}"
)
set_target_properties(yad PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
add_library(yad::core ALIAS yad)
target_include_directories(yad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(yad PUBLIC PkgConfig::CJSON)

# 2. POSIX + mbedtls3 platform library (for desktop yad_preview & G19 yad plugin)
if(YAD_BUILD_POSIX)
    list(APPEND CMAKE_PREFIX_PATH "/usr/lib/mbedtls3/cmake")
    find_package(MbedTLS REQUIRED)

    add_library(yad_posix STATIC
        src/platforms/posix/posix_platform.cpp
        src/platforms/posix/posix_net.cpp
        src/platforms/posix/local_sys.cpp
    )
    set_target_properties(yad_posix PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_library(yad::posix ALIAS yad_posix)
    target_include_directories(yad_posix PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "/usr/include/mbedtls3"
    )
    target_link_libraries(yad_posix PUBLIC
        yad
        MbedTLS::mbedtls
        MbedTLS::mbedx509
        MbedTLS::mbedcrypto
        dl
    )
endif()

# 3. Zero-Qt SDL3 preview application
if(YAD_BUILD_PREVIEW AND YAD_BUILD_POSIX)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

    add_executable(yad_preview apps/yad_preview/main.cpp)
    target_compile_definitions(yad_preview PRIVATE
        YAD_DEFAULT_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/configs/default_dashboard.json"
    )
    target_link_libraries(yad_preview PRIVATE
        yad_posix
        yad
        PkgConfig::SDL3
        PkgConfig::PNG
    )
endif()
