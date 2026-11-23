# Honeycomb C6 coprocessor — ESP-Hosted slave (Wi-Fi + BLE) over SDIO.
# Flashed onto the on-board ESP32-C6 via host-driven OTA from the P4 (no kit).
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(honeycomb_c6_hosted)
