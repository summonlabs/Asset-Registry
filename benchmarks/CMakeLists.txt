# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0

add_executable(ar_benchmark_registry benchmark_registry.cpp)
set_target_properties(ar_benchmark_registry PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
target_link_libraries(ar_benchmark_registry PRIVATE AssetRegistry::AssetRegistry AssetRegistry::warnings
    AssetRegistry::sanitizer)
asset_registry_place_library(ar_benchmark_registry)

# Benchmarks are run explicitly, not as part of the test suite: they measure
# throughput rather than assert behaviour, and they take minutes at the large
# scale. The `small` configuration runs in seconds and is suitable for a smoke
# check.
add_custom_target(run_benchmarks
    COMMAND $<TARGET_FILE:ar_benchmark_registry> standard
    DEPENDS ar_benchmark_registry
    COMMENT "Running Asset Registry benchmarks at the standard scale")

add_custom_target(run_benchmarks_small
    COMMAND $<TARGET_FILE:ar_benchmark_registry> small
    DEPENDS ar_benchmark_registry
    COMMENT "Running Asset Registry benchmarks at the small scale")
