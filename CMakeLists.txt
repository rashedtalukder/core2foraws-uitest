cmake_minimum_required(VERSION 3.16)

set(CORE2FORAWS_UITEST_VERSION "1.0.0")

idf_component_register(
    SRCS "uitest.c"
    INCLUDE_DIRS "include"
    REQUIRES "lvgl" "esp_lvgl_port" "core2foraws-screenshot"
)
# Old LVGL API names fail to build instead of resolving through aliases
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_DISABLE_API_MAPPING)
target_compile_definitions(${COMPONENT_LIB} PUBLIC CORE2FORAWS_UITEST_VERSION="${CORE2FORAWS_UITEST_VERSION}")
