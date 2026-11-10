# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Boards are selected by target (CMakePresets.json: "default" = T-Display S3, "p4" = JC1060P470).
# Each target keeps its own component lock file, otherwise every switch would re-solve the dependencies.
if(DEFINED IDF_TARGET AND NOT IDF_TARGET STREQUAL "esp32s3")
    set(DEPENDENCIES_LOCK_FILE "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(DEPENDENCIES_LOCK_FILE)
    idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK_FILE}")
endif()
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(usbhost_cd)
