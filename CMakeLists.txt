# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

cmake_minimum_required(VERSION 3.25...3.31)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(cmake/validate_special_target.cmake)
include(cmake/version.cmake)
desktop_app_parse_version(Telegram/build/version)

if (NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(configuration_types_init 1)
endif()

project(Telegram
    LANGUAGES C CXX
    VERSION ${desktop_app_version_cmake}
    DESCRIPTION "Official Telegram Desktop messenger"
    HOMEPAGE_URL "https://desktop.telegram.org"
)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (configuration_types_init
    AND CMAKE_CONFIGURATION_TYPES
    AND NOT MinSizeRel IN_LIST CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES};MinSizeRel" CACHE STRING "" FORCE)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Telegram)

get_filename_component(third_party_loc "Telegram/ThirdParty" REALPATH)
get_filename_component(submodules_loc "Telegram" REALPATH)
get_filename_component(cmake_helpers_loc "cmake" REALPATH)

if (NOT DESKTOP_APP_USE_PACKAGED AND WIN32)
    set(Python3_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/python/Scripts/python)
endif()

include(cmake/variables.cmake)
include(cmake/nice_target_sources.cmake)
include(cmake/target_compile_options_if_exists.cmake)
include(cmake/target_link_frameworks.cmake)
include(cmake/target_link_options_if_exists.cmake)
include(cmake/init_target.cmake)
include(cmake/generate_target.cmake)
include(cmake/nuget.cmake)
include(cmake/validate_d3d_compiler.cmake)
include(cmake/target_prepare_qrc.cmake)

include(cmake/options.cmake)

include(cmake/external/qt/package.cmake)
if (APPLE AND NOT DESKTOP_APP_USE_PACKAGED)
    get_filename_component(qt_loc
        "${Qt${QT_VERSION_MAJOR}_DIR}/../../.." REALPATH)
    get_property(qt_imported_targets DIRECTORY PROPERTY IMPORTED_TARGETS)
    foreach (qt_target IN LISTS qt_imported_targets)
        if (NOT qt_target MATCHES "^Qt[56]::")
            continue()
        endif()
        get_target_property(qt_target_type ${qt_target} TYPE)
        if (NOT qt_target_type STREQUAL "STATIC_LIBRARY")
            continue()
        endif()
        get_target_property(qt_configurations ${qt_target} IMPORTED_CONFIGURATIONS)
        foreach (qt_configuration IN LISTS qt_configurations)
            set(qt_location_property IMPORTED_LOCATION_${qt_configuration})
            get_target_property(qt_location ${qt_target} ${qt_location_property})
            if (qt_location)
                get_filename_component(qt_location "${qt_location}" REALPATH)
                set_property(TARGET ${qt_target} PROPERTY ${qt_location_property} "${qt_location}")
            endif()
        endforeach()
    endforeach()
endif()

set(desktop_app_skip_libs
    glibmm
    variant
)

add_subdirectory(cmake)
add_subdirectory(Telegram)
