cmake_minimum_required(VERSION 3.16)

project(jtplay)

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
find_path(QUICKJS_INCLUDE_DIR NAMES quickjs.h PATH_SUFFIXES quickjs)
find_library(QUICKJS_LIBRARY NAMES quickjs PATH_SUFFIXES quickjs)

add_executable(jtplay
    src/main.cpp
    src/app.cpp
    src/app_browse.cpp
    src/app_home.cpp
    src/app_library.cpp
    src/app_photo.cpp
    src/app_watch.cpp
    src/artcache.cpp
    src/gamepad.cpp
    src/library.cpp
    src/player.cpp
    src/visualizer.cpp
    src/browse/dlna_source.cpp
    src/browse/fs_source.cpp
    src/browse/js_http.cpp
    src/browse/js_source.cpp
    src/net/http_client.cpp
    src/upnp/http.cpp
    src/upnp/ssdp.cpp
    src/upnp/dlna.cpp
    src/RmlUi/RmlUi_Backend_SDL_GL3.cpp
    src/RmlUi/RmlUi_Renderer_GL3.cpp
    src/RmlUi/RmlUi_Platform_SDL.cpp
)

target_compile_options(jtplay PRIVATE -Wall -Werror)
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
    Threads::Threads
    ${QUICKJS_LIBRARY}
    ${ORBIS_LIBRARIES}
    m
)

if(ORBIS)
    pkg_check_modules(EGL REQUIRED egl)
    pkg_check_modules(GL REQUIRED gl)
    pkg_check_modules(VULKAN REQUIRED vulkan)

    file(COPY ${CMAKE_SYSROOT}/usr/etc/ca-bundle.crt DESTINATION ${CMAKE_CURRENT_SOURCE_DIR})

    orbis_add_pkg(jtplay
        TITLE jtplay
        TITLE_ID BREW04327
        CONTENT_ID IV0000-BREW04327_00-JTPLAY0000000000
        ASSETS sce_sys assets fonts plugins ca-bundle.crt
        MODULES libEGL.so.1 libGL.so.1 libgallium-${EGL_VERSION}.so libvulkan.so.1
    )
endif()
