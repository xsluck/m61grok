cmake_minimum_required(VERSION 3.15)

include(proj.conf)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# User
sdk_add_compile_definitions(-DCONFIG_CLI_CMD_ENABLE)

# AP 配网模式的板子 IP / DHCP 池（dhcpd 组件按此宏设接口 IP + 发地址池 192.168.1.2~254）
sdk_add_compile_definitions(-DDHCPD_SERVER_IP=\"192.168.1.1\")

target_sources(app PRIVATE tunnel.c)

sdk_add_include_directories(.)

sdk_set_main_file(main.c)

project(m61grok)
