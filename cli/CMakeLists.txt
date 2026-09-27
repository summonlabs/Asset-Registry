# Copyright 2026 Summon Software Labs
# SPDX-License-Identifier: Apache-2.0

add_executable(asset_registry_cli main.cpp)
set_target_properties(asset_registry_cli PROPERTIES OUTPUT_NAME asset-registry)
target_link_libraries(asset_registry_cli PRIVATE AssetRegistry::AssetRegistry AssetRegistry::warnings
    AssetRegistry::sanitizer)
asset_registry_place_library(asset_registry_cli)

install(TARGETS asset_registry_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
