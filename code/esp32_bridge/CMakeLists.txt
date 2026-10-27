cmake_minimum_required(VERSION 3.16)

# 项目名：esp32_bridge
# 目录结构：
#   esp32_bridge/
#   ├── CMakeLists.txt      ← 就是本文件
#   ├── sdkconfig.defaults  ← 首次构建自动采用的默认配置
#   └── main/
#       ├── CMakeLists.txt
#       └── main.c
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_bridge)
