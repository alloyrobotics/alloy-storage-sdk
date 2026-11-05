# Copyright (c) 2026 Alloy Robotics
# SPDX-License-Identifier: MIT

if(CONFIG_ALLOY_STORAGE)
  zephyr_library_named(alloy_storage)

  zephyr_include_directories(include core)

  # Platform-independent logic (no Zephyr headers), kept apart so it can move to core/.
  zephyr_library_sources(
    core/alloy_csv.c
    core/alloy_meta.c
    core/alloy_policy.c
    core/alloy_pool.c
  )

  zephyr_library_sources(
    src/alloy_http.c
    src/alloy_net.c
    src/alloy_storage.c
  )

  zephyr_library_compile_definitions(ALLOY_STORAGE_VERSION="0.1.0")

  set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
  zephyr_library_include_directories(${gen_dir})
  generate_inc_file_for_target(alloy_storage
    ${CMAKE_CURRENT_SOURCE_DIR}/certs/ca-bundle.pem
    ${gen_dir}/ca-bundle.pem.inc
  )
elseif(CONFIG_ALLOY_STORAGE_CORE)
  # Portable core only (unit tests on native_sim).
  zephyr_library_named(alloy_storage_core)
  zephyr_include_directories(core)
  zephyr_library_sources(
    core/alloy_csv.c
    core/alloy_meta.c
    core/alloy_policy.c
    core/alloy_pool.c
  )
endif()
