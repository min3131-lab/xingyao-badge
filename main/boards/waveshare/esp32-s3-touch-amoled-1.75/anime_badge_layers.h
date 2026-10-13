// 自动生成，请通过 scripts/build_badge_layers.py 更新。
#pragma once
#include "lvgl.h"
#include "sdkconfig.h"

#define ANIME_BADGE_HEAD_X 0
#define ANIME_BADGE_HEAD_Y 0
#define ANIME_BADGE_HEAD_WIDTH 466
#define ANIME_BADGE_HEAD_HEIGHT 350
#define ANIME_BADGE_BODY_X 0
#define ANIME_BADGE_BODY_Y 240
#define ANIME_BADGE_BODY_WIDTH 466
#define ANIME_BADGE_BODY_HEIGHT 226

#if CONFIG_ANIME_BADGE_DESKTOP
#ifdef __cplusplus
extern "C" {
#endif
LV_IMAGE_DECLARE(anime_badge_head);
LV_IMAGE_DECLARE(anime_badge_body);
#ifdef __cplusplus
}
#endif
#endif
