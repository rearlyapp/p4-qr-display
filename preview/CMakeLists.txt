cmake_minimum_required(VERSION 3.16)
project(packing_ui_preview C)
set(CMAKE_C_STANDARD 11)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(../managed_components/lvgl__lvgl lvgl)
add_executable(packing_ui_preview
    preview.c
    ../main/packing_visual.c
    ../main/fonts/ui_bold_20.c
    ../main/fonts/ui_bold_28.c
    ../main/fonts/ui_bold_40.c
    ../main/fonts/ui_bold_48.c)
target_include_directories(packing_ui_preview PRIVATE . ../main)
target_compile_definitions(packing_ui_preview PRIVATE STOCKIN_UI_PREVIEW
    LOGO_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../main/assets/rearly_logo.png")
target_link_libraries(packing_ui_preview PRIVATE lvgl m)
