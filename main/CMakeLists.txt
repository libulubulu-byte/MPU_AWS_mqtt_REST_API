idf_component_register(
    SRCS "app_main.c"
         "app_cfg.c"
         "app_version.c"
         "device_id.c"
         "wifi_net.c"
         "webprov.c"
         "sht30.c"
         "lamp.c"
         # 传感器驱动层：零业务依赖，只 include ESP-IDF，拷到别的工程也能编
         "mpu6050.c"
         "dht22.c"
         "hcsr04.c"
         "pir.c"
         # 应用层：把四个驱动组织成采集任务 + 告警状态机，依赖本项目
         "env_sensors.c"
         "aws_iot.c"
         "rest_api.c"
         "ota_update.c"
         "time_sync.c"
         "alarm.c"
         "shadow.c"
         "console.c"
    INCLUDE_DIRS "."
    # esp_netif 提供 esp_netif_sntp_*（SNTP 客户端），不是只用来建 netif
    REQUIRES driver esp_wifi esp_event esp_netif esp_http_server nvs_flash
             mqtt json esp_timer mbedtls led_strip
             esp_http_client esp_https_ota app_update
)

# 版本号来自工程根目录的 version.cmake（唯一来源），在这里转成编译期宏。
# app_config.h 里不再硬编码 APP_FW_VERSION_*，两处漂移的问题从根本上消失。
#
# 注意 app_config.h 仍要 include 到这些宏，所以它同时保留一组"兜底默认值"，
# 万一在别的构建系统里直接编（不经 CMake）也不会报未定义。
include(${CMAKE_CURRENT_LIST_DIR}/../version.cmake)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    APP_FW_VERSION_MAJOR=${APP_FW_VERSION_MAJOR}
    APP_FW_VERSION_MINOR=${APP_FW_VERSION_MINOR}
    APP_FW_VERSION_PATCH=${APP_FW_VERSION_PATCH}
)
