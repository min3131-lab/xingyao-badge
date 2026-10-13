# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.4.0")
project(xiaozhi)

# 徽章用 LVGL 的二维码控件；两套组件都附带 Nayuki 实现，只链接 LVGL 的超集版本。
# 仅调整本项目的构建目标，不改受管理依赖源码，普通板型保持原配置。
if(CONFIG_ANIME_BADGE_DESKTOP AND CONFIG_LV_USE_QRCODE)
    idf_component_get_property(BADGE_EMOTE_LIB espressif2022__esp_emote_gfx COMPONENT_LIB)
    get_target_property(BADGE_EMOTE_SOURCES ${BADGE_EMOTE_LIB} SOURCES)
    list(FILTER BADGE_EMOTE_SOURCES EXCLUDE REGEX "/qrcodegen\\.c$")
    set_property(TARGET ${BADGE_EMOTE_LIB} PROPERTY SOURCES "${BADGE_EMOTE_SOURCES}")
endif()
