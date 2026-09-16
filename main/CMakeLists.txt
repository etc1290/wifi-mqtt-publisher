idf_component_register(SRCS "main.c" "dht11.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif mqtt driver)
