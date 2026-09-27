# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0

# The suite runs each executable plainly: no timeout, no watchdog, no
# process-kill-as-pass. A case that hangs is a defect to be diagnosed.

set(ASSET_REGISTRY_TEST_SOURCES
    test_main.cpp
    test_support.cpp)

add_library(asset_registry_test_support STATIC ${ASSET_REGISTRY_TEST_SOURCES})
target_include_directories(asset_registry_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    # The adversarial suite inspects decoded durable state through the internal
    # store accessor, which is the only way to assert what recovery actually
    # loaded without re-parsing the file in the test.
    PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asset_registry_test_support
    PUBLIC AssetRegistryStatic
    PRIVATE AssetRegistry::warnings AssetRegistry::sanitizer)

# Each test binary covers one concern so a failure names the area immediately.
set(ASSET_REGISTRY_TEST_TARGETS
    ar_test_values
    ar_test_codec
    ar_test_wire
    ar_test_state_machine
    ar_test_registration
    ar_test_mutations
    ar_test_durability
    ar_test_derive
    ar_test_documents
    ar_test_property
    ar_test_adversarial
    ar_test_concurrency)

set(ar_test_values_SOURCES test_values.cpp)
set(ar_test_codec_SOURCES test_codec.cpp)
set(ar_test_wire_SOURCES test_wire.cpp)
set(ar_test_state_machine_SOURCES test_state_machine.cpp)
set(ar_test_registration_SOURCES test_registration.cpp)
set(ar_test_mutations_SOURCES test_mutations.cpp)
set(ar_test_durability_SOURCES test_durability.cpp)
set(ar_test_derive_SOURCES test_derive.cpp)
set(ar_test_documents_SOURCES test_documents.cpp)
set(ar_test_property_SOURCES test_property.cpp)
set(ar_test_adversarial_SOURCES test_adversarial.cpp)
set(ar_test_concurrency_SOURCES test_concurrency.cpp)

foreach(test_target IN LISTS ASSET_REGISTRY_TEST_TARGETS)
    add_executable(${test_target} ${${test_target}_SOURCES})
    # The sanitizer interface is applied to the test binaries as well as to the library
    # they exercise: an instrumented library linked into an uninstrumented binary is a
    # mixed-annotation link, which the toolchain refuses outright.
    target_link_libraries(${test_target} PRIVATE asset_registry_test_support AssetRegistry::warnings
        AssetRegistry::sanitizer)
    # The adversarial suite inspects decoded durable state through the internal
    # store accessor, which is the only way to assert what recovery actually loaded
    # without re-parsing the file in the test.
    target_include_directories(${test_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME ${test_target} COMMAND ${test_target})
    set_tests_properties(${test_target} PROPERTIES
        LABELS "asset-registry"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

# The multiprocess suite spawns independent operating-system processes against one
# store, so it is its own binary: a crash or a hang there cannot be confused with a
# fault in the in-process suites.
add_executable(ar_test_multiprocess test_multiprocess.cpp test_support.cpp test_main.cpp)
target_link_libraries(ar_test_multiprocess PRIVATE AssetRegistryStatic AssetRegistry::warnings
    AssetRegistry::sanitizer)
add_test(NAME ar_test_multiprocess COMMAND ar_test_multiprocess)
set_tests_properties(ar_test_multiprocess PROPERTIES
    LABELS "asset-registry"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# The helper runs as a separate process under ar_test_multiprocess; it is a real
# program, not a fixture, so it is built and installed alongside the tests. It is
# also copied next to the test binary, because a multi-configuration generator
# keeps the two in the same directory while running the test from the directory
# that holds every configuration's output.
add_executable(ar_multiprocess_helper helper/ar_multiprocess_helper.cpp)
target_link_libraries(ar_multiprocess_helper PRIVATE AssetRegistryStatic AssetRegistry::warnings
    AssetRegistry::sanitizer)
add_custom_command(TARGET ar_test_multiprocess POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:ar_multiprocess_helper>
        $<TARGET_FILE_DIR:ar_test_multiprocess>
    COMMENT "Placing ar_multiprocess_helper next to ar_test_multiprocess")
