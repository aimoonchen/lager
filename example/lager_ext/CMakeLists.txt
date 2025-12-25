# lager_ext/CMakeLists.txt
# CMake configuration for the lager_ext project
#
# This directory contains the following sub-projects:
# 1. lager_ext (static library) - Core functionality library
# 2. Editor (executable) - Editor process (Process A), supports both QML and Widgets modes
# 3. Engine (executable) - Engine process (Process B)
# 4. lager_extTest (executable) - Test program

cmake_minimum_required(VERSION 3.16)

# ============================================================
# Dependency Path Configuration - Using third_party directory
# ============================================================

# lager (parent project directory)
set(LAGER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Lager include directory")

# Boost - Using third_party directory (header-only)
# Includes: Hana, Intrusive, Interprocess and their dependencies
set(Boost_INCLUDE_DIR "${LAGER_INCLUDE_DIR}/third_party/boost" CACHE PATH "Boost include directory")

# immer - Using third_party submodule
set(IMMER_INCLUDE_DIR "${LAGER_INCLUDE_DIR}/third_party/immer" CACHE PATH "Immer include directory")

# zug - Using third_party submodule
set(ZUG_INCLUDE_DIR "${LAGER_INCLUDE_DIR}/third_party/zug" CACHE PATH "Zug include directory")

# Qt6 configuration - Update this path to your Qt6 installation
set(Qt6_DIR "D:/QT/6.10.1/msvc2022_64/lib/cmake/Qt6" CACHE PATH "Qt6 cmake directory")

set(SDL2_DIR "D:/Github/vcpkg/installed/x64-windows/share/sdl2" CACHE PATH "SDL2 cmake directory")

# ============================================================
# Find Dependencies
# ============================================================

# Find Qt6
find_package(Qt6 COMPONENTS Core Concurrent Gui Widgets QUIET)
if(Qt6_FOUND)
    message(STATUS "Found Qt6: ${Qt6Core_VERSION}")

    # Check for QML components in Qt6
    find_package(Qt6 COMPONENTS Qml Quick QuickControls2 QUIET)
    if(Qt6Qml_FOUND AND Qt6Quick_FOUND AND Qt6QuickControls2_FOUND)
        set(QT_HAS_QML TRUE)
    else()
        set(QT_HAS_QML FALSE)
    endif()
endif()

# Boost - Use specified path directly, not find_package
if(NOT EXISTS "${Boost_INCLUDE_DIR}/boost/version.hpp")
    message(FATAL_ERROR "Boost not found at ${Boost_INCLUDE_DIR}. Please set Boost_INCLUDE_DIR correctly.")
endif()
message(STATUS "Using Boost from: ${Boost_INCLUDE_DIR}")

# ============================================================
# lager_ext Static Library
# ============================================================

set(lager_ext_LIB_SOURCES
    value.cpp
    erased_lens.cpp
    lager_lens.cpp
    at_lens.cpp
    json_pointer.cpp
    static_path.cpp
    diff_collector.cpp
    shared_state.cpp
    editor_engine.cpp
    multi_store_engine.cpp
    delta_undo_engine.cpp
)

set(lager_ext_LIB_HEADERS
    lager_ext/value.h
    lager_ext/scene_types.h
    lager_ext/erased_lens.h
    lager_ext/lager_lens.h
    lager_ext/at_lens.h
    lager_ext/json_pointer.h
    lager_ext/static_path.h
    lager_ext/path_utils.h
    lager_ext/diff_collector.h
    lager_ext/shared_state.h
    lager_ext/shared_value.h
    lager_ext/fast_shared_value.h
    lager_ext/editor_engine.h
    lager_ext/multi_store_engine.h
    lager_ext/delta_undo_engine.h
)

add_library(lager_ext STATIC ${lager_ext_LIB_SOURCES} ${lager_ext_LIB_HEADERS})

target_include_directories(lager_ext PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LAGER_INCLUDE_DIR}
)

# Third-party libraries use SYSTEM flag to suppress their internal warnings
target_include_directories(lager_ext SYSTEM PUBLIC
    ${IMMER_INCLUDE_DIR}
    ${ZUG_INCLUDE_DIR}
    ${Boost_INCLUDE_DIR}
)

# Compiler macro definitions
target_compile_definitions(lager_ext PUBLIC
    # zug uses std::variant instead of Boost.Variant
    ZUG_VARIANT_STD
    # Boost.Interprocess needs this macro to avoid linking date_time library
    BOOST_DATE_TIME_NO_LIB
)

target_compile_features(lager_ext PUBLIC cxx_std_20)

if(MSVC)
    # /utf-8: Use UTF-8 for source and execution character sets
    # /W3: Warning level 3
    # /Zc:preprocessor: Use standards-conforming preprocessor
    # /bigobj: Support large object files
    target_compile_options(lager_ext PRIVATE /W3 /Zc:preprocessor /bigobj /utf-8)
endif()

# ============================================================
# lager_extTest Executable (Test Program)
# ============================================================

add_executable(lager_extTest main.cpp)

target_link_libraries(lager_extTest PRIVATE lager_ext)

if(MSVC)
    target_compile_options(lager_extTest PRIVATE /W3 /Zc:preprocessor /bigobj /utf-8)
endif()

# ============================================================
# Engine Executable (Process B - Engine)
# ============================================================

# Engine uses shared_value_demo.cpp as main program
add_executable(Engine shared_value_demo.cpp)

target_link_libraries(Engine PRIVATE lager_ext)

if(MSVC)
    target_compile_options(Engine PRIVATE /W3 /Zc:preprocessor /bigobj /utf-8)
endif()

# ============================================================
# Editor Executable (Process A - Editor)
# Supports both QML and Widgets modes via EDITOR_USE_QML macro
# ============================================================

if(Qt6_FOUND)
    add_executable(Editor editor_main.cpp)

    set_target_properties(Editor PROPERTIES AUTOMOC ON)

    target_link_libraries(Editor PRIVATE
        lager_ext
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Widgets
    )

    # QML support
    if(QT_HAS_QML)
        target_link_libraries(Editor PRIVATE
            Qt6::Qml
            Qt6::Quick
            Qt6::QuickControls2
        )
        target_compile_definitions(Editor PRIVATE EDITOR_HAS_QML=1)
    endif()

    target_compile_definitions(Editor PRIVATE
        LAGER_EXT_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
    )

    if(MSVC)
        target_compile_options(Editor PRIVATE /W3 /Zc:preprocessor /bigobj /utf-8)
    endif()

    message(STATUS "Editor target configured with Qt6 support")
    if(QT_HAS_QML)
        message(STATUS "  - QML support: enabled")
    else()
        message(STATUS "  - QML support: disabled")
    endif()
else()
    message(STATUS "Editor target skipped: Qt6 not found")
endif()

# Note: Engine executable already includes SharedValue demo functionality
# Uncomment below if a separate demo is needed
#
# if(WIN32)
#     add_executable(SharedValueDemo shared_value_demo.cpp)
#     target_link_libraries(SharedValueDemo PRIVATE lager_ext)
#     if(MSVC)
#         target_compile_options(SharedValueDemo PRIVATE /W3 /Zc:preprocessor /bigobj)
#     endif()
# endif()

# ============================================================
# Add to examples target (if exists)
# ============================================================

if(TARGET examples)
    add_dependencies(examples lager_extTest Engine)
    if(TARGET Editor)
        add_dependencies(examples Editor)
    endif()
endif()

# ============================================================
# Output Configuration Information
# ============================================================

message(STATUS "")
message(STATUS "lager_ext Project Configuration:")
message(STATUS "  Boost include: ${Boost_INCLUDE_DIR}")
message(STATUS "  Immer include: ${IMMER_INCLUDE_DIR}")
message(STATUS "  Zug include: ${ZUG_INCLUDE_DIR}")
message(STATUS "  Lager include: ${LAGER_INCLUDE_DIR}")
if(Qt6_FOUND)
    message(STATUS "  Qt version: ${Qt6Core_VERSION}")
else()
    message(STATUS "  Qt6: not found")
endif()
message(STATUS "")
