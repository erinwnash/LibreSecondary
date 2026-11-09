cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

project(LibreSecondary VERSION 1.0.0 LANGUAGES CXX)

# Enforce C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Ensure position-independent code across all targets
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# OpenFX Support Library Configuration
# ---------------------------------------------------------------------------
set(OPENFX_DIR "" CACHE PATH "Path to OpenFX repository root (containing include/ and Support/)")

if(NOT OPENFX_DIR)
    message(STATUS "OPENFX_DIR not specified. Fetching OpenFX Support library from GitHub...")
    include(FetchContent)
    FetchContent_Declare(
        openfx
        GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openfx.git
        GIT_TAG main
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(openfx)
    if(NOT openfx_POPULATED)
        FetchContent_Populate(openfx)
        set(OPENFX_DIR "${openfx_SOURCE_DIR}")
    endif()
endif()

message(STATUS "Using OpenFX from: ${OPENFX_DIR}")

set(OPENFX_SUPPORT_SOURCES
    ${OPENFX_DIR}/Support/Library/ofxsCore.cpp
    ${OPENFX_DIR}/Support/Library/ofxsImageEffect.cpp
    ${OPENFX_DIR}/Support/Library/ofxsInteract.cpp
    ${OPENFX_DIR}/Support/Library/ofxsLog.cpp
    ${OPENFX_DIR}/Support/Library/ofxsMultiThread.cpp
    ${OPENFX_DIR}/Support/Library/ofxsParams.cpp
    ${OPENFX_DIR}/Support/Library/ofxsProperty.cpp
    ${OPENFX_DIR}/Support/Library/ofxsPropertyValidation.cpp
)

add_library(openfx-cplusplus STATIC ${OPENFX_SUPPORT_SOURCES})

target_include_directories(openfx-cplusplus PUBLIC
    ${OPENFX_DIR}/include
    ${OPENFX_DIR}/Support/include
)

if(MSVC)
    target_compile_definitions(openfx-cplusplus PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _WINDOWS
        WIN32
        NOMINMAX
    )
    target_compile_options(openfx-cplusplus PRIVATE /W3 /MP)
else()
    target_compile_options(openfx-cplusplus PRIVATE
        -Wall
        -Wextra
        -fvisibility=hidden
    )
endif()

# ---------------------------------------------------------------------------
# LibreSecondary OFX Plugin Target
# ---------------------------------------------------------------------------
add_library(LibreSecondary MODULE
    SecondaryKeyer.cpp
    SecondaryKeyer.h
)

target_link_libraries(LibreSecondary PRIVATE openfx-cplusplus)

target_include_directories(LibreSecondary PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OPENFX_DIR}/include
    ${OPENFX_DIR}/Support/include
)

# Standard OpenFX plugin naming conventions
set_target_properties(LibreSecondary PROPERTIES
    PREFIX ""
    SUFFIX ".ofx"
)

if(MSVC)
    target_compile_definitions(LibreSecondary PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WINDOWS
        WIN32
        NOMINMAX
    )
    target_compile_options(LibreSecondary PRIVATE /W3 /MP)
else()
    target_compile_options(LibreSecondary PRIVATE
        -Wall
        -Wextra
        -fvisibility=hidden
    )
endif()

# ---------------------------------------------------------------------------
# Platform-specific OFX Bundle Directory Packaging
# ---------------------------------------------------------------------------
if(WIN32)
    set(OFX_ARCH_DIR "Win64")
elseif(APPLE)
    set(OFX_ARCH_DIR "MacOS")
else()
    set(OFX_ARCH_DIR "Linux-x86-64")
endif()

set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/LibreSecondary.ofx.bundle/Contents/${OFX_ARCH_DIR}")

add_custom_command(TARGET LibreSecondary POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:LibreSecondary>" "${BUNDLE_DIR}/"
    COMMENT "Packaging LibreSecondary into OFX bundle at ${BUNDLE_DIR}"
)

# ---------------------------------------------------------------------------
# Installation Targets
# ---------------------------------------------------------------------------
# Default OFX installation paths:
# Linux:   /usr/OFX/Plugins/
# Windows: C:/Program Files/Common Files/OFX/Plugins/
# macOS:   /Library/OFX/Plugins/
if(WIN32)
    set(DEFAULT_OFX_INSTALL_DIR "C:/Program Files/Common Files/OFX/Plugins")
elseif(APPLE)
    set(DEFAULT_OFX_INSTALL_DIR "/Library/OFX/Plugins")
else()
    set(DEFAULT_OFX_INSTALL_DIR "/usr/OFX/Plugins")
endif()

set(OFX_INSTALL_DIR "${DEFAULT_OFX_INSTALL_DIR}" CACHE PATH "Directory to install OFX plugins")

install(DIRECTORY "${CMAKE_BINARY_DIR}/LibreSecondary.ofx.bundle"
    DESTINATION "${OFX_INSTALL_DIR}"
    COMPONENT plugin
)
