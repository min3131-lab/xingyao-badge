#pragma once
#include "sdkconfig.h"
#if CONFIG_ANIME_BADGE_DESKTOP
#include <esp_heap_caps.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <array>
#include <cstring>
#include "anime_badge_expressions.h"

// 在渲染阶段加柔和边缘，避免独立表情图层露出矩形接缝；不改变原始素材。
class AnimeBadgeFaceMask {
public:
    ~AnimeBadgeFaceMask() { Reset(); }
    void Initialize() {
        const lv_image_dsc_t* source[] = {&anime_badge_blink_patch, &anime_badge_happy_patch,
                                          &anime_badge_shy_patch};
        const int width = ANIME_BADGE_FACE_WIDTH;
        const int height = ANIME_BADGE_FACE_HEIGHT;
        const size_t count = width * height;
        for (size_t i = 0; i < frames_.size(); ++i) {
            auto& frame = frames_[i];
            frame.source = source[i];
            frame.pixels = static_cast<uint8_t*>(
                heap_caps_malloc(count * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!frame.pixels)
                continue;
            memcpy(frame.pixels, source[i]->data, count * 2);
            auto alpha = frame.pixels + count * 2;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    int distance =
                        std::min(std::min(x, width - 1 - x), std::min(y, height - 1 - y));
                    float t = std::min(1.0f, distance / 16.0f);
                    // 平滑阶跃在边缘的斜率为零，叠加肤色不会形成一条硬线。
                    alpha[y * width + x] = static_cast<uint8_t>(255.0f * t * t * (3.0f - 2.0f * t));
                }
            }
            frame.image = *source[i];
            frame.image.header.cf = LV_COLOR_FORMAT_RGB565A8;
            frame.image.data_size = count * 3;
            frame.image.data = frame.pixels;
        }
    }
    const lv_image_dsc_t* Get(const lv_image_dsc_t* source) const {
        for (const auto& frame : frames_)
            if (frame.source == source && frame.pixels)
                return &frame.image;
        return source;
    }
    void Reset() {
        for (auto& frame : frames_) {
            if (frame.pixels) {
                lv_image_cache_drop(&frame.image);
                heap_caps_free(frame.pixels);
                frame = {};
            }
        }
    }

private:
    struct Frame {
        const lv_image_dsc_t* source = nullptr;
        uint8_t* pixels = nullptr;
        lv_image_dsc_t image = {};
    };
    std::array<Frame, 3> frames_{};
};
#endif
