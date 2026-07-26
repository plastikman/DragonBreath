# DragonBreath — open firmware for the BIGTREETECH Panda Breath (ESP32-C3)
cmake_minimum_required(VERSION 3.16)
# The shared board-agnostic core (event log, Wi-Fi/provisioning, Moonraker client)
# is now vendored locally as components/pb_evlog, pb_wifi, pb_moonraker — auto-
# discovered from components/, so no EXTRA_COMPONENT_DIRS is needed. It derives from
# the OpenVent family (justinh-rahb/OpenVent, MIT); see VENDORING.md for provenance.
# App version stamped into the ESP app descriptor (esp_app_get_description()->version,
# also shown in the web UI): the exact git tag when HEAD is tagged (releases), else
# the short commit hash (PR / local builds), with "-dirty" if the tree is modified.
execute_process(
    COMMAND git describe --tags --exact-match
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DB_TAG OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE DB_TAG_RC)
if(DB_TAG_RC EQUAL 0 AND DB_TAG)
    set(PROJECT_VER "${DB_TAG}")
else()
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DB_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE DB_HASH_RC)
    execute_process(
        COMMAND git status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DB_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT DB_HASH_RC EQUAL 0)
        set(PROJECT_VER "unknown")
    elseif(DB_DIRTY)
        set(PROJECT_VER "${DB_HASH}-dirty")
    else()
        set(PROJECT_VER "${DB_HASH}")
    endif()
endif()
message(STATUS "DragonBreath version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dragonbreath)

# E1 (Phase E) — warnings-as-errors for OUR OWN first-party components only.
# Applied here (after project(), when the per-component library targets exist) as a
# PRIVATE option so it never leaks to ESP-IDF's own components or to dependents.
# ESP-IDF framework code is deliberately excluded (we do not control its warning
# cleanliness). Both the default and release builds must stay clean under these flags.
#
# The pb_* components are auto-discovered from components/ (so a new one is covered
# automatically), and we assert at least one matched: if ESP-IDF ever changes the
# internal component-target naming (__idf_<name>), this fails the configure loudly
# instead of silently applying -Werror to nothing.
file(GLOB DB_OWN_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components/pb_*)
set(DB_WERROR_APPLIED 0)
foreach(db_dir ${DB_OWN_COMPONENT_DIRS})
    if(IS_DIRECTORY ${db_dir})
        get_filename_component(db_comp ${db_dir} NAME)
        if(TARGET __idf_${db_comp})
            target_compile_options(__idf_${db_comp} PRIVATE -Wall -Wextra -Werror)
            math(EXPR DB_WERROR_APPLIED "${DB_WERROR_APPLIED} + 1")
        endif()
    endif()
endforeach()
if(DB_WERROR_APPLIED EQUAL 0)
    message(FATAL_ERROR "E1: -Werror gate matched ZERO pb_* components — "
                        "the ESP-IDF component-target naming may have changed")
endif()
message(STATUS "E1: -Wall -Wextra -Werror applied to ${DB_WERROR_APPLIED} first-party components")
