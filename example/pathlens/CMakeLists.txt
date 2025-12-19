
# pathlens/CMakeLists.txt
# PathLens 项目的 CMake 配置
#
# 该目录包含以下子项目：
# 1. pathlens (静态库) - 核心功能库
# 2. Editor (可执行文件) - 编辑器进程 (A进程)，支持 QML 和 Widgets 两种模式
# 3. Engine (可执行文件) - 引擎进程 (B进程)
# 4. PathLensTest (可执行文件) - 测试程序

cmake_minimum_required(VERSION 3.16)

# ============================================================
# 依赖库路径配置
# ============================================================

# Boost
set(BOOST_ROOT "D:/Github/vcpkg/installed/x64-windows" CACHE PATH "Boost root directory")
set(Boost_INCLUDE_DIR "${BOOST_ROOT}/include" CACHE PATH "Boost include directory")
set(Boost_LIBRARY_DIR "${BOOST_ROOT}/lib" CACHE PATH "Boost library directory")

# immer
set(IMMER_INCLUDE_DIR "D:/Github/immer" CACHE PATH "Immer include directory")

# zug
set(ZUG_INCLUDE_DIR "D:/Github/zug" CACHE PATH "Zug include directory")

# lager (父项目目录)
set(LAGER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Lager include directory")

# Qt5
set(Qt5_DIR "D:/QT/5.15.2/msvc2019_64/lib/cmake/Qt5" CACHE PATH "Qt5 cmake directory")

# SDL2 (可选)
set(SDL2_DIR "D:/Github/vcpkg/installed/x64-windows/share/sdl2" CACHE PATH "SDL2 cmake directory")

# ============================================================
# 查找依赖
# ============================================================

# Qt5 组件
find_package(Qt5 COMPONENTS Core Concurrent Gui Widgets Qml QuickControls2 QUIET)

# Boost - 直接使用指定路径，不使用 find_package
# find_package(Boost 1.56 REQUIRED)  # 禁用，改用手动指定路径
if(NOT EXISTS "${Boost_INCLUDE_DIR}/boost/version.hpp")
    message(FATAL_ERROR "Boost not found at ${Boost_INCLUDE_DIR}. Please set Boost_INCLUDE_DIR correctly.")
endif()
message(STATUS "Using Boost from: ${Boost_INCLUDE_DIR}")

# ============================================================
# pathlens 静态库
# ============================================================

set(PATHLENS_LIB_SOURCES
    value.cpp
    erased_lens.cpp
    lager_lens.cpp
    at_lens.cpp
    json_pointer.cpp
    static_path.cpp
    diff_collector.cpp
    shared_state.cpp
    editor_engine.cpp
)

set(PATHLENS_LIB_HEADERS
    value.h
    erased_lens.h
    lager_lens.h
    at_lens.h
    json_pointer.h
    static_path.h
    diff_collector.h
    shared_state.h
    shared_value.h
    editor_engine.h
)

add_library(pathlens STATIC ${PATHLENS_LIB_SOURCES} ${PATHLENS_LIB_HEADERS})

target_include_directories(pathlens PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LAGER_INCLUDE_DIR}
)

# 第三方库使用 SYSTEM 标记，抑制其内部警告
target_include_directories(pathlens SYSTEM PUBLIC
    ${IMMER_INCLUDE_DIR}
    ${ZUG_INCLUDE_DIR}
    ${Boost_INCLUDE_DIR}
)

target_compile_features(pathlens PUBLIC cxx_std_20)

if(MSVC)
    # /utf-8: 源文件和执行字符集都使用 UTF-8
    # /W3: 警告级别 3
    # /Zc:preprocessor: 使用符合标准的预处理器
    # /bigobj: 支持大对象文件
    target_compile_options(pathlens PRIVATE /W3 /Zc:preprocessor /bigobj /utf-8)
endif()

# ============================================================
# PathLensTest 可执行文件 (测试程序)
# ============================================================

add_executable(PathLensTest main.cpp)

target_link_libraries(PathLensTest PRIVATE pathlens)

if(MSVC)
    target_compile_options(PathLensTest PRIVATE /W3 /Zc:preprocessor /bigobj /utf-8)
endif()

# ============================================================
# Engine 可执行文件 (B进程 - 引擎)
# ============================================================

# Engine 使用 shared_value_demo.cpp 作为主程序
add_executable(Engine shared_value_demo.cpp)

target_link_libraries(Engine PRIVATE pathlens)

if(MSVC)
    target_compile_options(Engine PRIVATE /W3 /Zc:preprocessor /bigobj /utf-8)
endif()

# ============================================================
# Editor 可执行文件 (A进程 - 编辑器)
# 支持 QML 和 Widgets 两种模式，通过 EDITOR_USE_QML 宏开关
# ============================================================

if(Qt5_FOUND)
    # Editor 主程序源文件
    add_executable(Editor editor_main.cpp)
    
    set_target_properties(Editor PROPERTIES AUTOMOC ON)
    
    target_link_libraries(Editor PRIVATE 
        pathlens
        Qt5::Core 
        Qt5::Concurrent
        Qt5::Gui 
        Qt5::Widgets
    )
    
    # QML 支持（如果 Qml 和 QuickControls2 可用）
    if(Qt5Qml_FOUND AND Qt5QuickControls2_FOUND)
        target_link_libraries(Editor PRIVATE
            Qt5::Qml 
            Qt5::QuickControls2
        )
        target_compile_definitions(Editor PRIVATE EDITOR_HAS_QML=1)
    endif()
    
    # 定义 QML 文件目录
    target_compile_definitions(Editor PRIVATE
        LAGER_PATHLENS_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
    )
    
    if(MSVC)
        target_compile_options(Editor PRIVATE /W3 /Zc:preprocessor /bigobj /utf-8)
    endif()
    
    message(STATUS "Editor target configured with Qt support")
    if(Qt5Qml_FOUND AND Qt5QuickControls2_FOUND)
        message(STATUS "  - QML support: enabled")
    else()
        message(STATUS "  - QML support: disabled (missing Qt5Qml or Qt5QuickControls2)")
    endif()
else()
    message(STATUS "Editor target skipped: Qt5 not found")
endif()

# 注意: Engine 可执行文件已经包含了 SharedValue 功能演示
# 如果需要单独的 Demo，可以取消下面的注释
# 
# if(WIN32)
#     add_executable(SharedValueDemo shared_value_demo.cpp)
#     target_link_libraries(SharedValueDemo PRIVATE pathlens)
#     if(MSVC)
#         target_compile_options(SharedValueDemo PRIVATE /W3 /Zc:preprocessor /bigobj)
#     endif()
# endif()

# ============================================================
# 添加到 examples 目标 (如果存在)
# ============================================================

if(TARGET examples)
    add_dependencies(examples PathLensTest Engine)
    if(TARGET Editor)
        add_dependencies(examples Editor)
    endif()
endif()

# ============================================================
# 输出配置信息
# ============================================================

message(STATUS "")
message(STATUS "PathLens Project Configuration:")
message(STATUS "  Boost include: ${Boost_INCLUDE_DIR}")
message(STATUS "  Immer include: ${IMMER_INCLUDE_DIR}")
message(STATUS "  Zug include: ${ZUG_INCLUDE_DIR}")
message(STATUS "  Lager include: ${LAGER_INCLUDE_DIR}")
if(Qt5_FOUND)
    message(STATUS "  Qt5 version: ${Qt5Core_VERSION}")
endif()
message(STATUS "")
