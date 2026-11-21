# Honeycomb — P4 host application (ESP32-P4)
# Build:  idf.py set-target esp32p4 && idf.py build
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_OPTIONS "-w" APPEND)
project(honeycomb-p4)
