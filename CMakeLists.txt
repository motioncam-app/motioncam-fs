cmake_minimum_required(VERSION 3.16)

# CMake 4 prefers Boost's package config; distributions still commonly ship
# only the classic FindBoost metadata.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

if(APPLE)
  option(MOTIONCAMFUSE_MACOS_X86_64 "Build x86_64 on macOS (Intel / Rosetta)" OFF)
  option(MOTIONCAMFUSE_MACOS_UNIVERSAL "Build universal macOS binary (x86_64 + arm64)" OFF)

  if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    if(MOTIONCAMFUSE_MACOS_UNIVERSAL)
      set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS" FORCE)
    elseif(MOTIONCAMFUSE_MACOS_X86_64)
      set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build architectures for macOS" FORCE)
    endif()
  endif()

  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum OS X deployment version")
endif()

project(MotionCamFuse VERSION 1.0 LANGUAGES CXX)

# The compiler is selected by the caller before project(); CI explicitly uses
# Clang and this makes the resulting compile database available to developers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

set(APP_NAME "MotionCam Fuse")
set(DEPENDENCIES_PATH deps)

# set(Boost_DEBUG 1)

set(MACOS_BUNDLE_ICON_FILE app_icon.icns)

set(PROJECT_SOURCES
        src/main.cpp
        src/mainwindow.cpp
        src/settingsdialog.cpp
        src/VirtualFileSystemImpl_MCRAW.cpp
        src/CameraMetadata.cpp
        src/CameraFrameMetadata.cpp
        src/AudioWriter.cpp
        src/Utils.cpp

        include/mainwindow.h
        include/settingsdialog.h
        include/Types.h
        include/IVirtualFileSystem.h
        include/IFuseFileSystem.h
        include/VirtualFileSystemImpl_MCRAW.h
        include/LRUCache.h
        include/AudioWriter.h
        include/Measure.h
        include/SingleApplication.h
        include/CameraMetadata.h
        include/CameraFrameMetadata.h
        include/Utils.h

        ui/mainwindow.ui
)

qt_add_resources(PROJECT_SOURCES
    resources.qrc
    qdarkstyle/dark/darkstyle.qrc)

if(WIN32)
    list(APPEND PROJECT_SOURCES
        resources/app.rc

        src/win/FuseFileSystemImpl_Win.cpp
        src/win/virtualizationInstance.cpp
        src/win/dirInfo.cpp

        include/win/dirInfo.h
        include/win/FuseFileSystemImpl_Win.h
        include/win/virtualizationInstance.h)

    find_library(projected-fs ProjectedFSLib)

    set(platform-specific ${projected-fs})
elseif(APPLE)
  list(APPEND PROJECT_SOURCES
      ${MACOS_BUNDLE_ICON_FILE}
      src/CrashDebug.cpp
      src/macos/FuseFileSystemImpl_MacOS.cpp
      include/CrashDebug.h
      include/macos/FuseFileSystemImpl_MacOS.h)

  find_library(FUSE_T_FRAMEWORK fuse_t)
  if(NOT FUSE_T_FRAMEWORK)
    find_library(FUSE_LIBRARY fuse)
    if(FUSE_LIBRARY)
      set(FUSE_T_FRAMEWORK ${FUSE_LIBRARY})
    else()
      find_library(FUSE3_LIBRARY fuse3)
      if(FUSE3_LIBRARY)
        set(FUSE_T_FRAMEWORK ${FUSE3_LIBRARY})
      endif()
    endif()
  endif()

  if(NOT FUSE_T_FRAMEWORK)
    message(FATAL_ERROR "macFUSE not found. Install macFUSE or set FUSE_T_FRAMEWORK.")
  endif()

  set(platform-specific ${FUSE_T_FRAMEWORK})

elseif(UNIX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)

  list(APPEND PROJECT_SOURCES
      src/linux/FuseFileSystemImpl_Linux.cpp
      include/linux/FuseFileSystemImpl_Linux.h)

  set(platform-specific PkgConfig::FUSE3)

endif()

set(CMAKE_AUTOUIC_SEARCH_PATHS ui)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(${PROJECT_NAME}
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(${PROJECT_NAME} SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(${PROJECT_NAME}
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE include)

target_compile_definitions(${PROJECT_NAME} PRIVATE _FILE_OFFSET_BITS=64)
if(APPLE)
  target_compile_definitions(${PROJECT_NAME} PRIVATE FUSE_USE_VERSION=26)
endif()

# # Debug configuration with sanitizers
# if(CMAKE_BUILD_TYPE STREQUAL "Debug")
#     target_compile_options(${PROJECT_NAME} PRIVATE
#         -fsanitize=address,undefined
#         -fsanitize-address-use-after-scope
#         -fno-sanitize-recover=all
#         -fstack-protector-strong
#         -g
#         -O0
#         -fno-omit-frame-pointer  # Better stack traces
#     )

#     target_link_options(${PROJECT_NAME} PRIVATE
#       -fsanitize=address,undefined
#       -fsanitize-address-use-after-scope
#       -fno-sanitize-recover=all
#       -fstack-protector-strong
#     )
# endif()

# Add motioncam decoder as static library
add_library(motioncam-decoder STATIC
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/Decoder.cpp
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/RawData.cpp
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/RawData_Legacy.cpp)

# The pinned decoder revision uses std::memcpy/std::memset in its legacy
# decoder without directly including <cstring>. Keep this compatibility fix in
# the parent project so a clean submodule checkout also builds with Clang.
target_precompile_headers(motioncam-decoder PRIVATE <cstring>)

target_include_directories(motioncam-decoder
    PUBLIC
    ${DEPENDENCIES_PATH}/bw64/include
    ${DEPENDENCIES_PATH}/motioncam-decoder/thirdparty
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/include
)

# Find the packages supplied by the active toolchain/package manager.
# Linux CI installs these through the host package manager; vcpkg provides the
# same config packages on Windows and macOS.
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)  # Explicitly find fmt as well

if(APPLE)
  if(TARGET spdlog::spdlog_header_only)
    set(MOTIONCAM_SPDLOG_TARGET spdlog::spdlog_header_only)
  else()
    set(MOTIONCAM_SPDLOG_TARGET spdlog::spdlog)
  endif()

  if(TARGET fmt::fmt-header-only)
    set(MOTIONCAM_FMT_TARGET fmt::fmt-header-only)
  else()
    set(MOTIONCAM_FMT_TARGET fmt::fmt)
  endif()
else()
  set(MOTIONCAM_SPDLOG_TARGET spdlog::spdlog)
  set(MOTIONCAM_FMT_TARGET fmt::fmt)
endif()

# Link spdlog to decoder to avoid ODR violations
target_link_libraries(motioncam-decoder PRIVATE ${MOTIONCAM_SPDLOG_TARGET} ${MOTIONCAM_FMT_TARGET})

# Add UTF-8 support for MSVC
if (MSVC)
    target_compile_options(motioncam-decoder PRIVATE /utf-8)
endif()

# Add boost
set(Boost_USE_STATIC_LIBS        ON)
set(Boost_USE_DEBUG_LIBS        OFF)
set(Boost_USE_RELEASE_LIBS       ON)
set(Boost_USE_MULTITHREADED      ON)
set(Boost_USE_STATIC_RUNTIME    OFF)

find_package(Boost 1.74 REQUIRED COMPONENTS filesystem locale regex)

# Header-only and pinned: fetching this is much quicker than a vcpkg setup.
include(FetchContent)
FetchContent_Declare(bs_thread_pool
    GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
    GIT_TAG v4.1.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(bs_thread_pool)
target_include_directories(${PROJECT_NAME} PRIVATE ${bs_thread_pool_SOURCE_DIR}/include)

target_link_libraries(${PROJECT_NAME} PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Network
  Boost::filesystem
  Boost::locale
  Boost::regex
  ${MOTIONCAM_SPDLOG_TARGET}
  ${MOTIONCAM_FMT_TARGET}
  motioncam-decoder
  ${platform-specific})

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE delayimp)
  target_link_options(${PROJECT_NAME} PRIVATE "/DELAYLOAD:projectedfslib.dll")
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/Enable_ProjFS.bat"
      "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Enable_ProjFS.bat"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/Disable_ProjFS.bat"
      "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Disable_ProjFS.bat"
  )
endif()

set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.motioncam.fuse")

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.motioncam.fuse)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE_ICON_FILE ${MACOS_BUNDLE_ICON_FILE}
    MACOSX_BUNDLE_BUNDLE_NAME ${APP_NAME}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

set_source_files_properties(${MACOS_BUNDLE_ICON_FILE} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(${PROJECT_NAME})
    
    # Deploy Qt dependencies using Qt's deployment tool
    if(WIN32)
        # Get Qt installation directory
        get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
        
        # Use windeployqt to copy all required Qt dependencies
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${QT_BIN_DIR}/windeployqt.exe"
                --debug
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-compiler-runtime
                --verbose 2
                $<TARGET_FILE:${PROJECT_NAME}>
        )
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${QT_BIN_DIR}/windeployqt.exe"
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-compiler-runtime
                --verbose 2
                $<TARGET_FILE:${PROJECT_NAME}>
        )       # add both release and debug qt dlls because im lazy
    endif()
endif()
