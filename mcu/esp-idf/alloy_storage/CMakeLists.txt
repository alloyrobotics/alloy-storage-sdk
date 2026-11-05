idf_component_register(
    SRCS "src/alloy_storage.c"
         "src/alloy_wifi.c"
         "portable/meta.c"
         "portable/policy.c"
         "portable/pool.c"
         "portable/record.c"
    INCLUDE_DIRS "include" "portable/include"
    REQUIRES esp_http_client esp_netif
    PRIV_REQUIRES esp_wifi esp_event esp_timer esp_app_format esp_hw_support lwip mbedtls nvs_flash log)
