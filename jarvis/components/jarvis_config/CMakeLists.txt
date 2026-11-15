idf_component_register(
    SRCS "JarvisConfig.cpp" "JarvisAgenda.cpp" "JarvisIcs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    PRIV_REQUIRES freertos esp_http_server esp_http_client esp_netif mbedtls esp-tls
)
