// 自动生成，请通过 scripts/build_badge_expressions.py 更新。
#pragma once
#include "lvgl.h"
#include "sdkconfig.h"

#define ANIME_BADGE_FACE_X 120
#define ANIME_BADGE_FACE_Y 138
#define ANIME_BADGE_FACE_WIDTH 216
#define ANIME_BADGE_FACE_HEIGHT 162

#if CONFIG_ANIME_BADGE_DESKTOP
#ifdef __cplusplus
extern "C" {
#endif
LV_IMAGE_DECLARE(anime_badge_blink_patch);
LV_IMAGE_DECLARE(anime_badge_happy_patch);
LV_IMAGE_DECLARE(anime_badge_shy_patch);
#ifdef __cplusplus
}
#endif
#endif
