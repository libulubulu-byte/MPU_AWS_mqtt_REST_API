# =====================================================================
# 固件版本号的【唯一来源】——发版只改这一个文件
# =====================================================================
#
# 版本号必须在两个地方同时存在，而且必须一致：
#
#   1. 烧进镜像头的 "App version" 字段（CMake 的 PROJECT_VER）
#      → OTA 时 esp_https_ota_get_img_desc() 读到的 img.version
#   2. 编译进应用的 C 宏 APP_FW_VERSION_MAJOR/MINOR/PATCH
#      → 开机日志、REST 上报的 version 字段
#
# 两处手工维护必然会在某次发版时忘掉一处，后果不是编译错误而是
# 运行期才暴露的诡异行为："清单说 1.0.1、镜像头说 1.0.0"，
# 于是 perform_ota() 的保护逻辑把本该装的升级拦下来。
#
# 所以这里定义一次，CMakeLists.txt 用它当 PROJECT_VER，
# main/CMakeLists.txt 把它转成 -D 传给编译器（见那里 target_compile_definitions）。
#
# 改版本：只动下面一行。
#
# 1.0.1 = 上一个版本（只加了 REST 上报与 OTA 闭环）。
# 1.0.2 = 本次：切 AWS 后端 + Device Shadow 闭环 + SNTP 校时 + 离线闹钟。
#
# ⚠️ 为什么必须 >= 1.0.1 而不能留在 1.0.0：
# sdkconfig 里 APP_OTA_CHECK_URL 指向的测试清单（ota.json）公告的是 1.0.1，
# 而上电检查的规则是"服务器更高就升级"。若本机版本停在 1.0.0，
# 一旦那个 8081 静态服务器开着，设备就会把**旧固件**拉回来覆盖当前版本 ——
# 新功能会被静默回退，现象是"刷完还是老样子"。
# 现在 1.0.2 > 1.0.1，不升级（不降级），安全。
set(APP_FW_VERSION "1.0.2")

# 拆成三段供 C 宏使用。用正则解析 "MAJOR.MINOR.PATCH"，
# 格式写错的话 configure 阶段就会报错，而不是留到运行期。
if(NOT APP_FW_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR
        "APP_FW_VERSION must be MAJOR.MINOR.PATCH (e.g. 1.0.0), got: '${APP_FW_VERSION}'")
endif()
set(APP_FW_VERSION_MAJOR ${CMAKE_MATCH_1})
set(APP_FW_VERSION_MINOR ${CMAKE_MATCH_2})
set(APP_FW_VERSION_PATCH ${CMAKE_MATCH_3})
