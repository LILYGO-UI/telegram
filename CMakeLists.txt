cmake_minimum_required(VERSION 3.21)

include(cmake/LpmMetadata.cmake)
project(${LILYGO_UI_CMAKE_PROJECT_NAME} LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)
find_package(LilyGoUI CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(JPEG QUIET)

set(TELEGRAM_USE_LVGL_TJPGD OFF)
if(NOT JPEG_FOUND AND TARGET lvgl)
    get_target_property(_telegram_lvgl_is_imported lvgl IMPORTED)
    if(NOT _telegram_lvgl_is_imported)
        target_compile_definitions(lvgl PUBLIC LV_USE_TJPGD=1)
        set(TELEGRAM_USE_LVGL_TJPGD ON)
    endif()
endif()

option(TELEGRAM_DEMO "Use deterministic local Telegram data instead of TDLib" ${CM0_SIMULATOR})
set(TELEGRAM_API_ID "" CACHE STRING "Telegram application api_id (runtime TELEGRAM_API_ID takes precedence)")
set(TELEGRAM_API_HASH "" CACHE STRING "Telegram application api_hash (runtime TELEGRAM_API_HASH takes precedence)")
# Populate empty caches too, so existing lpm builds pick up exported credentials.
foreach(_telegram_api_setting IN ITEMS TELEGRAM_API_ID TELEGRAM_API_HASH)
    if("${${_telegram_api_setting}}" STREQUAL "" AND DEFINED ENV{${_telegram_api_setting}})
        set_property(CACHE ${_telegram_api_setting} PROPERTY VALUE "$ENV{${_telegram_api_setting}}")
    endif()
endforeach()
if(TELEGRAM_API_ID STREQUAL "")
    set(TELEGRAM_API_ID_VALUE 0)
elseif(TELEGRAM_API_ID MATCHES "^[1-9][0-9]*$")
    set(TELEGRAM_API_ID_VALUE "${TELEGRAM_API_ID}")
else()
    message(FATAL_ERROR "TELEGRAM_API_ID must be a positive integer")
endif()
string(REPLACE "\\" "\\\\" TELEGRAM_API_HASH_ESCAPED "${TELEGRAM_API_HASH}")
string(REPLACE "\"" "\\\"" TELEGRAM_API_HASH_ESCAPED "${TELEGRAM_API_HASH_ESCAPED}")
set(LILYGO_UI_TELEGRAM_DEMO ${TELEGRAM_DEMO})
if(CM0_SIMULATOR)
    set(_telegram_demo_image "${CMAKE_CURRENT_SOURCE_DIR}/assets/demo-photo.jpg")
else()
    set(_telegram_demo_image
        "${CMAKE_INSTALL_FULL_DATADIR}/${LILYGO_UI_PACKAGE_NAME}/demo-photo.jpg")
endif()
string(REPLACE "\\" "\\\\" TELEGRAM_DEMO_IMAGE_ESCAPED "${_telegram_demo_image}")
string(REPLACE "\"" "\\\"" TELEGRAM_DEMO_IMAGE_ESCAPED "${TELEGRAM_DEMO_IMAGE_ESCAPED}")

configure_file(
    cmake/app_identity.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/app_identity.h
    @ONLY)
configure_file(
    cmake/telegram_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/telegram_config.h)

add_library(lilygo_ui_telegram_core STATIC
    src/domain/telegram_account_model.cpp
    src/domain/telegram_model.cpp
    src/support/json.cpp)
target_include_directories(lilygo_ui_telegram_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_telegram_core PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_features(lilygo_ui_telegram_core PUBLIC cxx_std_17)

add_library(lilygo_ui_telegram_services STATIC
    src/pages/telegram/telegram_view_model.cpp
    src/services/media/image_decoder.cpp
    src/services/media/voice_player.cpp
    src/services/telegram/demo_telegram_client.cpp
    src/services/telegram/runtime_config.cpp
    src/services/telegram/tdlib_account_protocol.cpp
    src/services/telegram/tdlib_client.cpp)
target_include_directories(lilygo_ui_telegram_services PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_telegram_services PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_features(lilygo_ui_telegram_services PUBLIC cxx_std_17)
target_link_libraries(lilygo_ui_telegram_services PUBLIC
    lilygo_ui_telegram_core
    Threads::Threads
    ${CMAKE_DL_LIBS})
if(JPEG_FOUND)
    target_compile_definitions(lilygo_ui_telegram_services PRIVATE
        TELEGRAM_HAS_JPEG=1)
    target_link_libraries(lilygo_ui_telegram_services PRIVATE JPEG::JPEG)
    message(STATUS "Telegram: JPEG photo decoding enabled")
elseif(TELEGRAM_USE_LVGL_TJPGD)
    message(STATUS "Telegram: using LVGL's bounded baseline JPEG decoder")
else()
    message(WARNING
        "Telegram: libjpeg was not found; JPEG photo messages will show a capability error")
endif()

add_library(lilygo_ui_telegram_ui STATIC
    src/app.cpp
    src/components/components.cpp
    src/pages/telegram/lvgl_photo_decoder.cpp
    src/pages/telegram/telegram_account_view.cpp
    src/pages/telegram/telegram_view.cpp)
target_include_directories(lilygo_ui_telegram_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(lilygo_ui_telegram_ui PRIVATE
    -Wall -Wextra -Wpedantic)
target_compile_features(lilygo_ui_telegram_ui PUBLIC cxx_std_17)
if(TELEGRAM_USE_LVGL_TJPGD)
    target_compile_definitions(lilygo_ui_telegram_ui PRIVATE
        TELEGRAM_HAS_LVGL_TJPGD=1)
endif()
target_link_libraries(lilygo_ui_telegram_ui PUBLIC
    lilygo_ui_telegram_services
    LilyGoUI::Runtime)

add_executable(lilygo_ui_telegram src/main.cpp)
target_compile_options(lilygo_ui_telegram PRIVATE
    -Wall -Wextra -Wpedantic)
target_link_libraries(lilygo_ui_telegram PRIVATE lilygo_ui_telegram_ui)
set_target_properties(lilygo_ui_telegram PROPERTIES
    OUTPUT_NAME "${LILYGO_UI_EXECUTABLE_NAME}")

configure_file(data/launcher.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json @ONLY)
configure_file(data/application.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop @ONLY)
configure_file(data/application.metainfo.xml.in
    ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml @ONLY)

install(TARGETS lilygo_ui_telegram
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.json
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/launcher/apps
    COMPONENT app)
lilygo_ui_install_launcher_icon(
    "${LILYGO_UI_APP_ID}"
    "${CMAKE_CURRENT_SOURCE_DIR}/${LILYGO_UI_APP_ICON}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${LILYGO_UI_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    COMPONENT app)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/demo-photo.jpg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/${LILYGO_UI_PACKAGE_NAME}
    COMPONENT app)
lilygo_ui_install_font_licenses(
    "${CMAKE_INSTALL_DATAROOTDIR}/doc/${LILYGO_UI_PACKAGE_NAME}"
    app)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(telegram-json-tests tests/test_json.cpp)
    target_compile_options(telegram-json-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(telegram-json-tests PRIVATE
        lilygo_ui_telegram_core)
    add_test(NAME telegram-json COMMAND telegram-json-tests)

    add_executable(telegram-model-tests tests/test_telegram_model.cpp)
    target_compile_options(telegram-model-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(telegram-model-tests PRIVATE
        lilygo_ui_telegram_core
        Threads::Threads)
    add_test(NAME telegram-model COMMAND telegram-model-tests)

    add_executable(telegram-view-model-tests tests/test_telegram_view_model.cpp)
    target_compile_options(telegram-view-model-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(telegram-view-model-tests PRIVATE
        lilygo_ui_telegram_services)
    add_test(NAME telegram-view-model COMMAND telegram-view-model-tests)

    add_executable(telegram-image-decoder-limits-tests tests/test_image_decoder_limits.cpp)
    target_compile_options(telegram-image-decoder-limits-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(telegram-image-decoder-limits-tests PRIVATE
        lilygo_ui_telegram_services)
    add_test(NAME telegram-image-decoder-limits COMMAND telegram-image-decoder-limits-tests)

    add_executable(telegram-voice-identity-tests tests/test_telegram_voice_identity.cpp)
    target_compile_options(telegram-voice-identity-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(telegram-voice-identity-tests PRIVATE
        lilygo_ui_telegram_ui)
    add_test(NAME telegram-voice-identity COMMAND telegram-voice-identity-tests)

    if(TELEGRAM_USE_LVGL_TJPGD)
        add_executable(telegram-lvgl-photo-decoder-tests tests/test_lvgl_photo_decoder.cpp)
        target_compile_options(telegram-lvgl-photo-decoder-tests PRIVATE
            -Wall -Wextra -Wpedantic -UNDEBUG)
        target_link_libraries(telegram-lvgl-photo-decoder-tests PRIVATE lilygo_ui_telegram_ui)
        add_test(NAME telegram-lvgl-photo-decoder COMMAND telegram-lvgl-photo-decoder-tests
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/demo-photo.jpg)
    endif()

    add_executable(telegram-tdlib-mapping-tests tests/test_tdlib_mapping.cpp)
    target_compile_options(telegram-tdlib-mapping-tests PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(telegram-tdlib-mapping-tests PRIVATE
        lilygo_ui_telegram_services)
    add_test(NAME telegram-tdlib-mapping COMMAND telegram-tdlib-mapping-tests)

    add_executable(telegram-tdlib-account-tests tests/test_tdlib_account.cpp)
    target_compile_options(telegram-tdlib-account-tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(telegram-tdlib-account-tests PRIVATE lilygo_ui_telegram_services)
    add_test(NAME telegram-tdlib-account COMMAND telegram-tdlib-account-tests)

    add_executable(telegram-render-ui tests/render_ui.cpp)
    target_compile_options(telegram-render-ui PRIVATE
        -Wall -Wextra -Wpedantic -UNDEBUG)
    target_link_libraries(telegram-render-ui PRIVATE
        lilygo_ui_telegram_ui)
    foreach(viewport IN ITEMS portrait landscape)
        if(viewport STREQUAL "portrait")
            set(_width 568)
            set(_height 1232)
        else()
            set(_width 1232)
            set(_height 568)
        endif()
        add_test(NAME telegram-render-${viewport} COMMAND telegram-render-ui
            ${_width} ${_height}
            ${CMAKE_CURRENT_BINARY_DIR}/telegram-${viewport}.ppm)
        set_tests_properties(telegram-render-${viewport} PROPERTIES
            ENVIRONMENT "TELEGRAM_DEMO=1")
    endforeach()
endif()

# New applications have no legacy CM0 package to replace. Add Debian migration
# fields only when this project supersedes an actually published package.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${LILYGO_UI_VERSION}")
if(LILYGO_UI_VERSION MATCHES
   "^([0-9]+:)?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_2}")
    set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_3}")
    set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_4}")
else()
    set(CPACK_PACKAGE_VERSION_MAJOR 0)
    set(CPACK_PACKAGE_VERSION_MINOR 0)
    set(CPACK_PACKAGE_VERSION_PATCH 0)
endif()
set(CPACK_PACKAGE_CONTACT "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_VENDOR "${LILYGO_UI_PACKAGE_CONTACT}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${LILYGO_UI_APP_SUMMARY}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${LILYGO_UI_APP_HOMEPAGE}")
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_SET_DESTDIR ON)
set(CPACK_COMPONENTS_ALL app)
set(CPACK_DEBIAN_APP_PACKAGE_NAME "${LILYGO_UI_PACKAGE_NAME}")
set(CPACK_DEBIAN_APP_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_APP_PACKAGE_DEPENDS
    "libc6 (>= 2.38), libstdc++6 (>= 14.1), libgcc-s1, libdrm2, libfreetype6, libpng16-16, zlib1g, libbz2-1.0, libbrotli1, lilygo-ui-appkit-dev (>= ${LILYGO_UI_MIN_APPKIT_VERSION})")
if(JPEG_FOUND)
    string(APPEND CPACK_DEBIAN_APP_PACKAGE_DEPENDS ", libjpeg62-turbo")
endif()
if(NOT TELEGRAM_DEMO)
    string(APPEND CPACK_DEBIAN_APP_PACKAGE_DEPENDS ", libtdjson1.8.38")
endif()
set(CPACK_DEBIAN_APP_PACKAGE_RECOMMENDS
    "libasound2, libopusfile0")
set(CPACK_DEBIAN_APP_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        "${LILYGO_UI_PACKAGE_ARCHITECTURE}")
endif()
include(CPack)
