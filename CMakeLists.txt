cmake_minimum_required(VERSION 3.16)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project definition. Deliberately no VERSION: it would pin the version
# reported by esp_app_get_description(), and ESP-IDF otherwise derives it from
# `git describe`, so a release tag identifies the running firmware over OTA.
project(neato-mqtt-controller)
