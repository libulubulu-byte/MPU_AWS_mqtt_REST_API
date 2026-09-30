# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 版本号的唯一来源在 version.cmake；不设置 PROJECT_VER 时 IDF 会退回
# git describe（得到 "676dc691" 这种 hash），拿它当镜像头版本和
# APP_FW_VERSION 比较语义就全乱了。详见 version.cmake 的注释。
include(${CMAKE_CURRENT_LIST_DIR}/version.cmake)
set(PROJECT_VER "${APP_FW_VERSION}")

project(aws_mqtt)
