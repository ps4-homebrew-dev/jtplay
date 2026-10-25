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

target_include_directories(jtplay PRIVATE ${QUICKJS_INCLUDE_DIR} src)

if(ORBIS)
  set(ORBIS_LIBRARIES SceIpmi SceAppContent)
endif()

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
    set(TITLE "JTPlay")
    set(TITLE_ID "BREW04327")
    set(CONTENT_ID "IV0000-${TITLE_ID}_00-JTPLAY0000000000")
    set(VERSION "1.00")

    # The package is staged in pkg/, laid out exactly as it is installed:
    # pkg/ is /app0 on the console, where src/main.cpp looks for the assets,
    # fonts, plugins and CA bundle. jtplay.gp4 is generated there from the
    # same list, so whatever is staged is packaged.
    set(PKG_DIR ${CMAKE_CURRENT_BINARY_DIR}/pkg)
    set(SYSROOT_PREFIX ${CMAKE_SYSROOT}/${CMAKE_INSTALL_PREFIX})

    pkg_check_modules(EGL REQUIRED egl)
    set(MESA_MODULES
        libEGL.so.1
        libGL.so.1
        libgallium-${EGL_VERSION}.so
        libvulkan.so.1
    )

    set(PKG_FILES eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png ca-bundle.crt)
    set(PKG_OUTPUTS)

    add_custom_command(
        OUTPUT ${PKG_DIR}/eboot.bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}
        COMMAND ${ELF2SELF} $<TARGET_FILE:jtplay> ${PKG_DIR}/eboot.bin
        DEPENDS jtplay
    )

    add_custom_command(
        OUTPUT ${PKG_DIR}/sce_sys/param.sfo
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}/sce_sys
        COMMAND ${MKSFO} -i APP_TYPE=1 -s128 TITLE=${TITLE}
                         -s12 TITLE_ID=${TITLE_ID}
                         -s48 CONTENT_ID=${CONTENT_ID}
                         -s8 APP_VER=${VERSION}
                         -s8 VERSION=${VERSION}
                         -i ATTRIBUTE=2
                         -i DOWNLOAD_DATA_SIZE=0
                         -i SYSTEM_VER=0
                         -s4 CATEGORY=gd
                         ${PKG_DIR}/sce_sys/param.sfo
        VERBATIM
    )

    foreach(img icon0.png pic1.png)
        add_custom_command(
            OUTPUT ${PKG_DIR}/sce_sys/${img}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}/sce_sys
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/${img}
                    ${PKG_DIR}/sce_sys/${img}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/${img}
        )
    endforeach()

    add_custom_command(
        OUTPUT ${PKG_DIR}/ca-bundle.crt
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SYSROOT_PREFIX}/etc/ca-bundle.crt ${PKG_DIR}/ca-bundle.crt
        DEPENDS ${SYSROOT_PREFIX}/etc/ca-bundle.crt
    )

    # Mesa: libGL.so.1 is what RmlUi's GL loader opens.
    foreach(module ${MESA_MODULES})
        add_custom_command(
            OUTPUT ${PKG_DIR}/sce_module/${module}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_DIR}/sce_module
            COMMAND ${ELF2SELF} ${SYSROOT_PREFIX}/lib/${module}
                                ${PKG_DIR}/sce_module/${module}
            DEPENDS ${SYSROOT_PREFIX}/lib/${module}
        )
        list(APPEND PKG_FILES sce_module/${module})
    endforeach()

    # Every asset, font and plugin, as in the source tree (editor backups
    # left out).
    file(GLOB_RECURSE DATA_FILES CONFIGURE_DEPENDS
         RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
         ${CMAKE_CURRENT_SOURCE_DIR}/assets/*
         ${CMAKE_CURRENT_SOURCE_DIR}/fonts/*
         ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*)
    list(FILTER DATA_FILES EXCLUDE REGEX "~$")
    list(SORT DATA_FILES)
    foreach(file ${DATA_FILES})
        add_custom_command(
            OUTPUT ${PKG_DIR}/${file}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_CURRENT_SOURCE_DIR}/${file} ${PKG_DIR}/${file}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${file}
        )
        list(APPEND PKG_FILES ${file})
    endforeach()

    foreach(file ${PKG_FILES})
        list(APPEND PKG_OUTPUTS ${PKG_DIR}/${file})
    endforeach()

    add_custom_command(
        OUTPUT ${PKG_DIR}/jtplay.gp4
        COMMAND ${MKGP4} -c ${CONTENT_ID} -o jtplay.gp4 ${PKG_FILES}
        WORKING_DIRECTORY ${PKG_DIR}
        DEPENDS ${PKG_OUTPUTS}
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${CONTENT_ID}.pkg
        COMMAND ${MKPKG} -o ${CMAKE_CURRENT_BINARY_DIR}/${CONTENT_ID}.pkg jtplay.gp4
        WORKING_DIRECTORY ${PKG_DIR}
        DEPENDS ${PKG_DIR}/jtplay.gp4 ${PKG_OUTPUTS}
        VERBATIM
    )

    add_custom_target(pkg ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${CONTENT_ID}.pkg)
endif()
