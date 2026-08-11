idf_component_register(
    SRCS "led_ring.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_timer nvs_flash esp_hw_support
)

if(CONFIG_LED_RING_ENABLE)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${PROJECT_DIR}/main/rtsp"
    )
endif()
