cmake_minimum_required(VERSION 3.20)

project(AssetRegistry
    VERSION 1.0.0
    DESCRIPTION "DCCP Tranche 1 authoritative physical asset inventory and lifecycle registry"
    HOMEPAGE_URL "https://github.com/summonlabs/Asset-Registry"
    LANGUAGES CXX)

# The standard is set once, globally, so every target in every subdirectory and
# every consumer of this build tree compiles with the same language level. Setting
# it per target is easy to forget for a new target, and forgetting it silently
# downgrades the language level instead of failing.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ASSET_REGISTRY_IS_TOP_LEVEL ON)
else()
    set(ASSET_REGISTRY_IS_TOP_LEVEL OFF)
endif()

option(ASSET_REGISTRY_BUILD_SHARED "Build the asset registry library as a shared library" ON)
option(ASSET_REGISTRY_BUILD_TESTS "Build the asset registry test suite" ${ASSET_REGISTRY_IS_TOP_LEVEL})
option(ASSET_REGISTRY_BUILD_CLI "Build the asset-registry inspection CLI" ON)
option(ASSET_REGISTRY_BUILD_EXAMPLES "Build the asset registry examples" ${ASSET_REGISTRY_IS_TOP_LEVEL})
option(ASSET_REGISTRY_BUILD_BENCHMARKS "Build the asset registry benchmarks" ${ASSET_REGISTRY_IS_TOP_LEVEL})
option(ASSET_REGISTRY_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
set(ASSET_REGISTRY_SANITIZE "" CACHE STRING
    "Sanitizer instrumentation to apply to first-party targets (empty, 'address', or 'thread')")

if(NOT ASSET_REGISTRY_BUILD_SHARED)
    set(ASSET_REGISTRY_LIBRARY_TYPE STATIC)
else()
    set(ASSET_REGISTRY_LIBRARY_TYPE SHARED)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Shared warning / sanitizer interface
# ---------------------------------------------------------------------------

add_library(asset_registry_warnings INTERFACE)
add_library(AssetRegistry::warnings ALIAS asset_registry_warnings)

if(MSVC)
    target_compile_options(asset_registry_warnings INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /EHsc)
    # C4251 warns that standard-library members of an exported class are not
    # themselves exported. This library's public surface is deliberately made of
    # value types with standard-library members, so the warning fires on almost
    # every public type without describing a defect: consumers are built with a
    # compatible toolchain and standard library, and no standard-library object is
    # ever constructed on one side of the boundary and destroyed on the other. The
    # suppression is applied once, here, rather than per class.
    target_compile_options(asset_registry_warnings INTERFACE /wd4251)
    if(ASSET_REGISTRY_WARNINGS_AS_ERRORS)
        target_compile_options(asset_registry_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(asset_registry_warnings INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(asset_registry_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2)
    if(ASSET_REGISTRY_WARNINGS_AS_ERRORS)
        target_compile_options(asset_registry_warnings INTERFACE -Werror)
    endif()
endif()

set(ASSET_REGISTRY_SANITIZER_FLAGS "")
if(ASSET_REGISTRY_SANITIZE STREQUAL "address")
    if(MSVC)
        list(APPEND ASSET_REGISTRY_SANITIZER_FLAGS /fsanitize=address)
        # MSVC address sanitizer requires the dynamic runtime.
        set(ASSET_REGISTRY_MSVC_ASAN_RUNTIME ON)
    else()
        list(APPEND ASSET_REGISTRY_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    endif()
elseif(ASSET_REGISTRY_SANITIZE STREQUAL "thread")
    if(MSVC)
        message(FATAL_ERROR "ASSET_REGISTRY_SANITIZE=thread is not supported by MSVC")
    endif()
    list(APPEND ASSET_REGISTRY_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT ASSET_REGISTRY_SANITIZE STREQUAL "")
    message(FATAL_ERROR "Unsupported ASSET_REGISTRY_SANITIZE value: ${ASSET_REGISTRY_SANITIZE}")
endif()

add_library(asset_registry_sanitizer INTERFACE)
add_library(AssetRegistry::sanitizer ALIAS asset_registry_sanitizer)
if(ASSET_REGISTRY_SANITIZER_FLAGS)
    target_compile_options(asset_registry_sanitizer INTERFACE ${ASSET_REGISTRY_SANITIZER_FLAGS})
    if(MSVC)
        # MSVC records the sanitizer runtime in the instrumented objects' metadata and
        # treats /fsanitize=address as a compile option only, so passing it to the linker
        # makes the linker warn about an option it does not recognise. Incremental linking
        # is incompatible with that metadata, so it is turned off here rather than left for
        # the linker to refuse with a warning.
        target_link_options(asset_registry_sanitizer INTERFACE /INCREMENTAL:NO)
    else()
        # GCC and Clang select the sanitizer runtime at link time, so the flag belongs
        # there as well as on the compile line.
        target_link_options(asset_registry_sanitizer INTERFACE ${ASSET_REGISTRY_SANITIZER_FLAGS})
    endif()
endif()

# ---------------------------------------------------------------------------
# Core object library (all first-party translation units)
# ---------------------------------------------------------------------------

set(ASSET_REGISTRY_PUBLIC_HEADERS
    include/asset_registry/asset.hpp
    include/asset_registry/asset_class.hpp
    include/asset_registry/asset_id.hpp
    include/asset_registry/asset_registry.hpp
    include/asset_registry/authority.hpp
    include/asset_registry/capability.hpp
    include/asset_registry/error.hpp
    include/asset_registry/export.hpp
    include/asset_registry/lifecycle.hpp
    include/asset_registry/limits.hpp
    include/asset_registry/persistence.hpp
    include/asset_registry/provenance.hpp
    include/asset_registry/query.hpp
    include/asset_registry/reference.hpp
    include/asset_registry/registry_policy.hpp
    include/asset_registry/replacement.hpp
    include/asset_registry/serial_identity.hpp
    include/asset_registry/snapshot.hpp
    include/asset_registry/strong_types.hpp
    include/asset_registry/text.hpp)

set(ASSET_REGISTRY_INTERNAL_HEADERS
    src/crc32.hpp
    src/fingerprint.hpp
    src/fs_atomic.hpp
    src/json_encode.hpp
    src/json_parse.hpp
    src/registry_state.hpp
    src/snapshot_internal.hpp
    src/store_format.hpp
    src/store_internal.hpp
    src/tlv.hpp
    src/uuid.hpp)

set(ASSET_REGISTRY_SOURCES
    src/asset.cpp
    src/asset_class.cpp
    src/asset_registry.cpp
    src/authority.cpp
    src/capability.cpp
    src/crc32.cpp
    src/error.cpp
    src/export.cpp
    src/fs_atomic.cpp
    src/json_encode.cpp
    src/json_parse.cpp
    src/lifecycle.cpp
    src/limits.cpp
    src/persistence.cpp
    src/provenance.cpp
    src/reference.cpp
    src/registry_policy.cpp
    src/registry_state.cpp
    src/replacement.cpp
    src/serial_identity.cpp
    src/snapshot.cpp
    src/store_format.cpp
    src/strong_types.cpp
    src/text.cpp
    src/tlv.cpp
    src/uuid.cpp)

add_library(asset_registry_objects OBJECT ${ASSET_REGISTRY_SOURCES} ${ASSET_REGISTRY_INTERNAL_HEADERS})
set_target_properties(asset_registry_objects PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(asset_registry_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(asset_registry_objects
    PRIVATE AssetRegistry::warnings AssetRegistry::sanitizer)

if(ASSET_REGISTRY_MSVC_ASAN_RUNTIME)
    target_compile_options(asset_registry_objects PRIVATE /fsanitize=address)
endif()

if(WIN32 AND ASSET_REGISTRY_BUILD_SHARED)
    # The symbols are compiled into the object library, so the export and import
    # declarations must reach that target rather than only the library target that
    # collects its objects. Without ASSET_REGISTRY_BUILDING_LIBRARY here, every
    # declaration would carry an empty annotation, no symbol would be exported, and
    # the link would fail for want of an import library.
    target_compile_definitions(asset_registry_objects
        PRIVATE ASSET_REGISTRY_BUILDING_LIBRARY
        PUBLIC ASSET_REGISTRY_SHARED)
endif()

# ---------------------------------------------------------------------------
# Public library target
# ---------------------------------------------------------------------------

add_library(AssetRegistry ${ASSET_REGISTRY_LIBRARY_TYPE}
    $<TARGET_OBJECTS:asset_registry_objects>)
add_library(AssetRegistry::AssetRegistry ALIAS AssetRegistry)

set_target_properties(AssetRegistry PROPERTIES
    OUTPUT_NAME asset_registry
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(AssetRegistry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(AssetRegistry
    PRIVATE AssetRegistry::sanitizer AssetRegistry::warnings)

if(ASSET_REGISTRY_MSVC_ASAN_RUNTIME)
    target_compile_options(AssetRegistry PRIVATE /fsanitize=address)
endif()

if(WIN32)
    # BCryptGenRandom supplies the process entropy used for generated canonical
    # identities. It is part of the operating system, not a third-party
    # dependency.
    target_link_libraries(AssetRegistry PRIVATE bcrypt)
endif()

# Static build of the same objects, used only by the test suite and the benchmarks.
# It exists so tests can reach internal accessors (for example the decoded durable
# state a recovery produced) without exporting implementation detail from the
# shared library that consumers see. It is never installed and never exported.
add_library(AssetRegistryStatic STATIC $<TARGET_OBJECTS:asset_registry_objects>)
target_include_directories(AssetRegistryStatic
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(AssetRegistryStatic PRIVATE bcrypt)
endif()

# ---------------------------------------------------------------------------
# Installation and package export
# ---------------------------------------------------------------------------

install(TARGETS AssetRegistry
    EXPORT AssetRegistryTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/asset_registry
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

set(ASSET_REGISTRY_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/AssetRegistry)

install(EXPORT AssetRegistryTargets
    FILE AssetRegistryTargets.cmake
    NAMESPACE AssetRegistry::
    DESTINATION ${ASSET_REGISTRY_CMAKE_DIR})

configure_package_config_file(
    cmake/AssetRegistryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AssetRegistryConfig.cmake
    INSTALL_DESTINATION ${ASSET_REGISTRY_CMAKE_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AssetRegistryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/AssetRegistryConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AssetRegistryConfigVersion.cmake
    DESTINATION ${ASSET_REGISTRY_CMAKE_DIR})

install(FILES README.md LICENSE NOTICE CONTRIBUTING.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# CLI, examples, tests, benchmarks
# ---------------------------------------------------------------------------

# A multi-configuration generator puts the shared library in a per-configuration
# directory, and that directory is not on the loader's search path for an executable
# sitting in its own directory. First-party executables that link the shared library
# therefore get a copy placed beside them, which is what keeps the CLI and the example
# tests runnable straight from the build tree.
function(asset_registry_place_library target)
    if(ASSET_REGISTRY_BUILD_SHARED)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:AssetRegistry>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Placing the asset registry library beside ${target}")
    endif()
endfunction()

# CTest has to be enabled before any directory that registers a test, and the
# examples register tests as well as the test suite does. A directory added before
# enable_testing() has its add_test() calls dropped silently, so the ordering here
# is what keeps the examples inside `ctest`.
if(ASSET_REGISTRY_BUILD_TESTS OR ASSET_REGISTRY_BUILD_EXAMPLES)
    enable_testing()
endif()

if(ASSET_REGISTRY_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(ASSET_REGISTRY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ASSET_REGISTRY_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ASSET_REGISTRY_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
