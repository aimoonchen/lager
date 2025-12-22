cmake_minimum_required(VERSION 3.8)
cmake_policy(SET CMP0048 NEW) # enable project VERSION
cmake_policy(SET CMP0056 NEW) # honor link flags in try_compile()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

project(lager VERSION 0.1.0)

# C++20 标准设置 - 全局生效
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W3 /Zc:preprocessor /bigobj)
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS on)
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Qunused-arguments")
endif()

include(GNUInstallDirs)

option(lager_BUILD_TESTS "Build tests" ON)
option(lager_BUILD_FAILURE_TESTS "Build failure tests" ON)
option(lager_BUILD_EXAMPLES "Build examples" ON)
option(lager_BUILD_DEBUGGER_EXAMPLES "Build examples that showcase the web based debugger" ON)
option(lager_BUILD_DOCS "Build docs" ON)
option(lager_EMBED_RESOURCES_PATH "Embed installation paths for easier, non-portable resource location" ON)
option(lager_DISABLE_STORE_DEPENDENCY_CHECKS "Disable compile-time checks for store dependencies" OFF)
option(lager_ENABLE_EXCEPTIONS "Always enable exceptions regardless of detected compiler support" OFF)
option(lager_DISABLE_EXCEPTIONS "Always disable exceptions regardless of detected compiler support" OFF)

if (lager_ENABLE_EXCEPTIONS AND lager_DISABLE_EXCEPTIONS)
  message(FATAL_ERROR "Cannot both enable and disable exceptions")
endif()

if (NOT lager_EMBED_RESOURCES_PATH AND lager_BUILD_EXAMPLES)
  message(FATAL_ERROR "Examples require embedded resources path")
endif()

if (MSVC AND NOT lager_DISABLE_STORE_DEPENDENCY_CHECKS)
  message(WARNING "compile-time checks for store dependencies are currently not supported with msvc")
  set(lager_DISABLE_STORE_DEPENDENCY_CHECKS ON)
endif()

if (NOT lager_BUILD_EXAMPLES AND lager_BUILD_DEBUGGER_EXAMPLES)
    message(WARNING "examples using the web-based debugger are disabled when examples are disabled")
    set(lager_BUILD_DEBUGGER_EXAMPLES OFF)
endif()

find_program(CCACHE ccache)
if (CCACHE)
  message(STATUS "Using ccache: ${CCACHE}")
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ${CCACHE})
  set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ${CCACHE})
else()
  message(STATUS "Could not find ccache")
endif()

#  Targets
#  =======

# the library
add_library(lager INTERFACE)
target_include_directories(lager INTERFACE
  $<BUILD_INTERFACE:${lager_BINARY_DIR}/>
  $<BUILD_INTERFACE:${lager_SOURCE_DIR}/>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(lager_DISABLE_STORE_DEPENDENCY_CHECKS)
  message(STATUS "Disabling compile-time checks for store dependencies")
  target_compile_definitions(lager INTERFACE LAGER_DISABLE_STORE_DEPENDENCY_CHECKS)
endif()

if(lager_ENABLE_EXCEPTIONS)
  message(STATUS "Explicitly enabling exceptions")
  target_compile_definitions(lager INTERFACE LAGER_USE_EXCEPTIONS)
endif()

if(lager_DISABLE_EXCEPTIONS)
  message(STATUS "Explicitly disabling exceptions")
  target_compile_definitions(lager INTERFACE LAGER_NO_EXCEPTIONS)
endif()

install(TARGETS lager EXPORT LagerConfig)

# requirements for tests and examples
if (lager_BUILD_TESTS OR lager_BUILD_EXAMPLES)
  # ============================================================
  # 依赖库路径配置 - 使用 third_party 目录
  # ============================================================
  
  # Boost - 使用 third_party 目录 (header-only)
  # 包含: Hana, Intrusive, Interprocess 及其依赖
  set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost" CACHE PATH "Boost include directory")
  
  if(NOT EXISTS "${Boost_INCLUDE_DIR}/boost/version.hpp")
    message(FATAL_ERROR "Boost not found at ${Boost_INCLUDE_DIR}. Please ensure Boost headers are in third_party/boost/boost/")
  endif()
  message(STATUS "Using Boost from third_party: ${Boost_INCLUDE_DIR}")
  
  # Boost libraries (header-only, 无需链接库)
  set(Boost_LIBRARIES "")
  
  # 创建 Boost interface 目标
  if(NOT TARGET Boost::headers)
    add_library(Boost_headers INTERFACE)
    add_library(Boost::headers ALIAS Boost_headers)
    target_include_directories(Boost_headers INTERFACE ${Boost_INCLUDE_DIR})
  endif()
  
  # immer - 使用 third_party 子模块
  set(IMMER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/immer" CACHE PATH "Immer include directory")
  if(NOT EXISTS "${IMMER_INCLUDE_DIR}/immer/vector.hpp")
    message(FATAL_ERROR "Immer submodule not found. Please run: git submodule update --init --recursive")
  endif()
  message(STATUS "Using Immer from submodule: ${IMMER_INCLUDE_DIR}")
  
  # zug - 使用 third_party 子模块
  set(ZUG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zug" CACHE PATH "Zug include directory")
  if(NOT EXISTS "${ZUG_INCLUDE_DIR}/zug/compose.hpp")
    message(FATAL_ERROR "Zug submodule not found. Please run: git submodule update --init --recursive")
  endif()
  message(STATUS "Using Zug from submodule: ${ZUG_INCLUDE_DIR}")
  
  # 创建 immer 和 zug 的 interface 目标（如果不存在）
  if(NOT TARGET immer)
    add_library(immer INTERFACE)
    target_include_directories(immer INTERFACE ${IMMER_INCLUDE_DIR})
  endif()
  
  if(NOT TARGET zug)
    add_library(zug INTERFACE)
    target_include_directories(zug INTERFACE ${ZUG_INCLUDE_DIR})
    # 使用 C++ 标准库的 std::variant 而不是 Boost.Variant
    target_compile_definitions(zug INTERFACE ZUG_VARIANT_STD)
  endif()
  
  find_package(Threads REQUIRED)
endif()

# the library, local development target
if(lager_BUILD_TESTS)
  # Catch2 - 使用指定路径
  set(CATCH2_INCLUDE_DIR "D:/Github/vcpkg/installed/x64-windows/include" CACHE PATH "Catch2 include directory")
  if(NOT EXISTS "${CATCH2_INCLUDE_DIR}/catch2/catch_all.hpp" AND NOT EXISTS "${CATCH2_INCLUDE_DIR}/catch.hpp")
    message(FATAL_ERROR "Catch2 not found at ${CATCH2_INCLUDE_DIR}. Please set CATCH2_INCLUDE_DIR correctly.")
  endif()
  message(STATUS "Using Catch2 from: ${CATCH2_INCLUDE_DIR}")
  
  # 创建 Catch2 interface 目标（如果不存在）
  if(NOT TARGET Catch2::Catch2)
    add_library(Catch2 INTERFACE)
    add_library(Catch2::Catch2 ALIAS Catch2)
    target_include_directories(Catch2 INTERFACE ${CATCH2_INCLUDE_DIR})
  endif()

  add_library(lager-dev INTERFACE)
  target_include_directories(lager-dev SYSTEM INTERFACE
    ${lager_SOURCE_DIR}
    ${Boost_INCLUDE_DIR}
  )
  target_link_libraries(lager-dev INTERFACE lager
    ${Boost_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    immer
    zug
  )
  if (ENABLE_COVERAGE)
    target_compile_options(lager-dev INTERFACE "--coverage")
    target_link_libraries(lager-dev INTERFACE "--coverage")
  endif()
  if (ENABLE_ASAN)
    target_compile_options(lager-dev INTERFACE
      -fno-omit-frame-pointer -fsanitize=address)
    target_link_options(lager-dev INTERFACE -fsanitize=address)
  endif()

  enable_testing()
  add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Build and run all the tests and examples.")

  add_subdirectory(test)
endif()

# the library, with http debugger
if (lager_BUILD_EXAMPLES)

  add_library(lager-example INTERFACE)
  target_include_directories(lager-example INTERFACE
     $<BUILD_INTERFACE:${lager_BINARY_DIR}/>
     $<BUILD_INTERFACE:${lager_SOURCE_DIR}/>
    ${Boost_INCLUDE_DIR}
    ${IMMER_INCLUDE_DIR}
    ${ZUG_INCLUDE_DIR}
  )
  target_link_libraries(lager-example INTERFACE
    lager
    ${CMAKE_THREAD_LIBS_INIT}
    ${Boost_LIBRARIES}
  )

  # 注意: lager-example 不导出，因为它依赖本地配置的路径
  # install(TARGETS lager-example EXPORT LagerConfig)

  # examples with the http debugger
  if (lager_BUILD_DEBUGGER_EXAMPLES)
    # 创建 Boost::system 目标（如果不存在）
    if(NOT TARGET Boost::system)
      add_library(Boost_system INTERFACE)
      add_library(Boost::system ALIAS Boost_system)
      target_include_directories(Boost_system INTERFACE ${Boost_INCLUDE_DIR})
      # Boost system 在新版本中是 header-only
    endif()
    
    add_library(lager-debugger-example INTERFACE)
    target_link_libraries(lager-debugger-example INTERFACE
      lager-example
      Boost::system)

    if (NOT APPLE)
      add_custom_target(gui ALL
        COMMAND make
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/resources/gui"
        COMMENT "Build debugger web UI")
    endif()

    # 注意: lager-debugger-example 不导出
    # install(TARGETS lager-debugger-example EXPORT LagerConfig)

    install(FILES
      resources/gui/gui.js
      resources/gui/gui.css
      resources/gui/index.html
      DESTINATION "${CMAKE_INSTALL_DATADIR}/lager/gui")
    endif()

  add_subdirectory(example)
endif()

if (lager_BUILD_DOCS)
  add_subdirectory(doc)
endif()

# Also configure and install the resources_path.hpp file if wanted
if (lager_EMBED_RESOURCES_PATH)
  set(LAGER_PREFIX_PATH ${CMAKE_INSTALL_PREFIX})
  configure_file(lager/resources_path.hpp.in
    ${CMAKE_BINARY_DIR}/include/lager/resources_path.hpp)
  target_include_directories(lager
    INTERFACE $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>)
  install(FILES ${CMAKE_BINARY_DIR}/include/lager/resources_path.hpp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lager")
endif()

install(EXPORT LagerConfig DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Lager")
install(DIRECTORY lager DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" PATTERN "*.hpp")
