# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0

add_executable(ar_example_lifecycle lifecycle_walkthrough.cpp)
target_link_libraries(ar_example_lifecycle PRIVATE AssetRegistry::AssetRegistry AssetRegistry::warnings
    AssetRegistry::sanitizer)
asset_registry_place_library(ar_example_lifecycle)

add_executable(ar_example_export_consumer export_consumer.cpp)
target_link_libraries(ar_example_export_consumer PRIVATE AssetRegistry::AssetRegistry AssetRegistry::warnings
    AssetRegistry::sanitizer)
asset_registry_place_library(ar_example_export_consumer)

# The examples are part of validation, not decoration: each runs as a test, and the
# walkthrough then validates its own store with the CLI.
add_test(NAME ar_example_lifecycle COMMAND ar_example_lifecycle
    "${CMAKE_CURRENT_BINARY_DIR}/example-store")
set_tests_properties(ar_example_lifecycle PROPERTIES LABELS "asset-registry")

add_test(NAME ar_example_export_round_trip
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:asset_registry_cli>
        -DCONSUMER=$<TARGET_FILE:ar_example_export_consumer>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/example-round-trip
        -P ${CMAKE_CURRENT_SOURCE_DIR}/export_round_trip.cmake)
set_tests_properties(ar_example_export_round_trip PROPERTIES LABELS "asset-registry")
