cmake_minimum_required(VERSION 3.16)

project(jtplay VERSION 1.1.0 LANGUAGES C CXX)

option(JTPLAY_WERROR "Treat compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)
find_package(RmlUi REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(FREETYPE2 REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)
pkg_check_modules(TINYXML2 REQUIRED IMPORTED_TARGET tinyxml2)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)
find_path(QUICKJS_INCLUDE_DIR NAMES quickjs.h PATH_SUFFIXES quickjs)
find_library(QUICKJS_LIBRARY NAMES quickjs PATH_SUFFIXES quickjs)
if(NOT QUICKJS_INCLUDE_DIR OR NOT QUICKJS_LIBRARY)
    message(FATAL_ERROR
        "QuickJS not found (quickjs.h and libquickjs.a, each in a directory "
        "'quickjs' under include/ and lib/). See Building in README.md.")
endif()

# The JavaScript plugin host, on its own: what the app and the headless
# plugin runner (and through it the tests) share.
set(JTPLAY_PLUGIN_HOST_SOURCES
    src/browse/js_http.cpp
    src/browse/js_plugin.cpp
    src/browse/js_source.cpp
    src/net/http_client.cpp
    src/plugin_cli.cpp
)

add_executable(jtplay
    src/main.cpp
    src/app.cpp
    src/app_browse.cpp
    src/app_home.cpp
    src/app_library.cpp
    src/app_photo.cpp
    src/app_sheet.cpp
    src/app_watch.cpp
    src/artcache.cpp
    src/gamepad.cpp
    src/library.cpp
    src/player.cpp
    src/visualizer.cpp
    src/browse/dlna_source.cpp
    src/browse/fs_source.cpp
    ${JTPLAY_PLUGIN_HOST_SOURCES}
    src/upnp/http.cpp
    src/upnp/ssdp.cpp
    src/upnp/dlna.cpp
    src/RmlUi/RmlUi_Backend_SDL_GL3.cpp
    src/RmlUi/RmlUi_Renderer_GL3.cpp
    src/RmlUi/RmlUi_Platform_SDL.cpp
)

target_compile_options(jtplay PRIVATE -Wall)
if(JTPLAY_WERROR)
    target_compile_options(jtplay PRIVATE -Werror)
endif()
target_compile_definitions(jtplay PRIVATE JTPLAY_VERSION="${PROJECT_VERSION}")
# Antialiasing of the UI's edges (rounded corners, curves): the renderer's
# default of 2 samples leaves them visibly stepped. Clamped to what the
# driver supports (RmlUi_Renderer_GL3.cpp).
target_compile_definitions(jtplay PRIVATE RMLUI_NUM_MSAA_SAMPLES=4)
target_include_directories(jtplay PRIVATE ${QUICKJS_INCLUDE_DIR} src)

target_link_libraries(jtplay PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    RmlUi::Core
    PkgConfig::FREETYPE2
    PkgConfig::MPV
    PkgConfig::SDL2_IMAGE
    PkgConfig::TINYXML2
    PkgConfig::CURL
    PkgConfig::OPENSSL
    Threads::Threads
    ${QUICKJS_LIBRARY}
    ${ORBIS_LIBRARIES}
    m
)

# jtplay-plugin: the plugin runner as a program of its own ("jtplay --plugin"
# without the window, the player and their libraries), for trying plugins
# out on a PC. Not for the console: there is nowhere to run it.
if(NOT ORBIS)
    add_executable(jtplay-plugin ${JTPLAY_PLUGIN_HOST_SOURCES})
    target_compile_options(jtplay-plugin PRIVATE -Wall)
    if(JTPLAY_WERROR)
        target_compile_options(jtplay-plugin PRIVATE -Werror)
    endif()
    target_compile_definitions(jtplay-plugin PRIVATE
        JTPLAY_PLUGIN_TOOL JTPLAY_VERSION="${PROJECT_VERSION}")
    target_include_directories(jtplay-plugin PRIVATE ${QUICKJS_INCLUDE_DIR} src)
    target_link_libraries(jtplay-plugin PRIVATE
        PkgConfig::TINYXML2 PkgConfig::CURL PkgConfig::OPENSSL
        Threads::Threads ${QUICKJS_LIBRARY} m)
endif()

if(ORBIS)
    pkg_check_modules(EGL REQUIRED egl)
    pkg_check_modules(GL REQUIRED gl)
    pkg_check_modules(VULKAN REQUIRED vulkan)

    file(COPY ${CMAKE_SYSROOT}/usr/etc/ca-bundle.crt DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})

    # A non-game application ("gdc"), which a PS5 running it as a PS4 app
    # treats as a media app: only that gets the remote's media keys there.
    # A non-game has no /temp0 (temporary data, games only), so it asks for
    # download data instead, mounted at /download0, where SDL2main puts the
    # XDG base directories (the cache, settings and library: see README).
    # Its size is in MiB.
    orbis_add_pkg(jtplay
        TITLE jtplay
        TITLE_ID BREW04327
        CONTENT_ID IV0000-BREW04327_00-JTPLAY0000000000
        CATEGORY gdc
        DOWNLOAD_DATA_SIZE 512
        # 0x2 | 0x10000 (7 CPU mode) | 0x800000 (PS4 Pro mode)
        ATTRIBUTE 8454146
        ASSETS sce_sys assets fonts plugins ca-bundle.crt
        MODULES libEGL.so.1 libGL.so.1 libgallium-${EGL_VERSION}.so libvulkan.so.1
    )
endif()
