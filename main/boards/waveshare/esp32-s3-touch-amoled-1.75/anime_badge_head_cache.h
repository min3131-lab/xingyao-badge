#pragma once

#include "sdkconfig.h"

#if CONFIG_ANIME_BADGE_DESKTOP

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <src/misc/cache/lv_cache.h>
#if LV_USE_LZ4_INTERNAL
#include <src/libs/lz4/lz4.h>
#endif
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "anime_badge_layers.h"
#include "assets.h"

// 调用 Initialize、Get、Reset 以及析构时由显示层持有 LVGL 锁。
// 释放缓存前，调用方必须先让图像对象切回原图或删除图像对象。
class AnimeBadgeHeadCache {
public:
    AnimeBadgeHeadCache() = default;
    AnimeBadgeHeadCache(const AnimeBadgeHeadCache&) = delete;
    AnimeBadgeHeadCache& operator=(const AnimeBadgeHeadCache&) = delete;

    ~AnimeBadgeHeadCache() { Reset(); }

    bool Initialize() {
        if (frame_count_)
            return true;
        if (!lv_display_get_default())
            return false;
        if (InitializeBank())
            return true;

        // 上部只覆盖黑色背景，使用 RGB565 直接复制；颈部保留透明，继续与身体混合。
        ScratchCanvas scratch;
        if (!scratch.Initialize()) {
            ESP_LOGW("AnimeBadge", "头部缓存未启用：无法创建临时画布");
            return false;
        }
        // 首选每半度一档，让扩大后的摆头仍细腻；内存不足时逐级回退。
        if (!AllocateFrames(13) && !AllocateFrames(9) && !AllocateFrames(5)) {
            ESP_LOGW("AnimeBadge", "头部缓存未启用：PSRAM 不足，保留原图显示");
            return false;
        }

        const lv_area_t image_area = {0, 0, ANIME_BADGE_HEAD_WIDTH - 1,
                                      ANIME_BADGE_HEAD_HEIGHT - 1};
        for (size_t i = 0; i < frame_count_; ++i) {
            auto& frame = frames_[i];
            lv_canvas_fill_bg(scratch.canvas, lv_color_black(), LV_OPA_TRANSP);

            lv_layer_t layer;
            lv_canvas_init_layer(scratch.canvas, &layer);
            lv_draw_rect_dsc_t background;
            lv_draw_rect_dsc_init(&background);
            background.bg_color = lv_color_black();
            background.bg_opa = LV_OPA_COVER;
            const lv_area_t opaque_area = {0, 0, ANIME_BADGE_HEAD_WIDTH - 1,
                                           static_cast<int32_t>(kOpaqueHeight) - 1};
            lv_draw_rect(&layer, &background, &opaque_area);
            lv_draw_image_dsc_t draw;
            lv_draw_image_dsc_init(&draw);
            draw.src = &anime_badge_head;
            // 与 lv_image_set_rotation 的角度归一化保持一致，贴片才能准确配准。
            draw.rotation = (kPoseCorrection + frame.angle + 3600) % 3600;
            draw.pivot = {233, 320};
            draw.antialias = true;
            lv_draw_image(&layer, &draw, &image_area);
            lv_canvas_finish_layer(scratch.canvas, &layer);
            ConvertFrame(scratch.draw_buf, frame.draw_buf, frame.lower_buf);

            // 通过 LVGL 的公开转换函数生成独立图像描述符，不转换 header 指针。
            lv_draw_buf_to_image(&frame.draw_buf, &frame.image);
            lv_draw_buf_to_image(&frame.lower_buf, &frame.lower_image);
            // 预绘制可能持续数秒，每帧让出 CPU，避免长时间阻塞空闲任务。
            vTaskDelay(1);
        }
        scratch.Reset();

        if (heap_caps_get_free_size(kMemoryCaps) < kReservedBytes) {
            // 构建期间其他任务也可能申请 PSRAM，发布缓存前再次检查预留空间。
            ESP_LOGW("AnimeBadge", "头部缓存已释放：构建后 PSRAM 预留不足");
            Reset();
            return false;
        }
        ESP_LOGI("AnimeBadge", "头部预绘制缓存：%u 档，占用 %u 字节，PSRAM 剩余 %u 字节",
                 static_cast<unsigned>(frame_count_), static_cast<unsigned>(allocated_bytes_),
                 static_cast<unsigned>(heap_caps_get_free_size(kMemoryCaps)));
        return true;
    }

    // 角度单位为 0.1 度。调用方把返回的 actual_angle 同步应用到表情贴片。
    // 姿态库不可用时退回自然歪头附近的小幅缓存，不强制纠正原图姿态。
    const lv_image_dsc_t* Get(int angle, int& actual_angle) {
        actual_angle = kPoseCorrection;
        if (!frame_count_)
            return nullptr;
        if (bank_count_) {
            SelectBankFrame(angle);
        } else {
            active_frame_ = NearestFrame(angle);
        }
        actual_angle += frames_[active_frame_].angle;
        return &frames_[active_frame_].image;
    }

    const lv_image_dsc_t* GetLower(int /* angle */) const {
        // Get 已经选定整帧；下半层不能再次选档，避免双缓冲上下部来自不同角度。
        return frame_count_ ? &frames_[active_frame_].lower_image : nullptr;
    }

    static constexpr int LowerY() { return kOpaqueHeight; }
    static constexpr int PoseCorrection() { return kPoseCorrection; }

    size_t FrameCount() const { return bank_count_ ? bank_count_ : frame_count_; }

    void ReportDecodeMetrics() {
        if (!decode_count_)
            return;
        ESP_LOGI("AnimeBadge", "姿态解压：次数=%u 平均=%.2fms 最长=%.2fms",
                 static_cast<unsigned>(decode_count_),
                 static_cast<double>(decode_total_us_) / decode_count_ / 1000.0,
                 static_cast<double>(decode_max_us_) / 1000.0);
        decode_count_ = decode_max_us_ = 0;
        decode_total_us_ = 0;
    }

    void Reset() {
        for (auto& frame : frames_) {
            if (!frame.pixels)
                continue;
            DropFrameCache(frame);
            heap_caps_free(frame.pixels);
            frame = {};
        }
        frame_count_ = 0;
        allocated_bytes_ = 0;
        active_frame_ = bank_count_ = 0;
        bank_data_ = nullptr;
        decode_count_ = decode_max_us_ = 0;
        decode_total_us_ = 0;
    }

private:
    struct Frame {
        uint8_t* pixels = nullptr;
        lv_draw_buf_t draw_buf = {};
        lv_draw_buf_t lower_buf = {};
        lv_image_dsc_t image = {};
        lv_image_dsc_t lower_image = {};
        int angle = 0;
    };

    struct BankEntry {
        int angle = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t crc = 0;
    };

    static constexpr uint32_t kMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    // 零度就是原图的自然歪头；回正和另一侧倾斜由六轴控制，不能永久纠偏。
    static constexpr int kPoseCorrection = 0;
    // 身体起点前保留 16 像素余量，较大跟随动作时黑底仍不会盖住肩颈。
    static constexpr uint32_t kOpaqueHeight = ANIME_BADGE_BODY_Y - 16;
    static constexpr uint32_t kLowerHeight = ANIME_BADGE_HEAD_HEIGHT - kOpaqueHeight;
    static constexpr size_t kReservedBytes = 2 * 1024 * 1024;
    static constexpr size_t kAlignment =
        LV_DRAW_BUF_ALIGN < sizeof(void*) ? sizeof(void*) : LV_DRAW_BUF_ALIGN;
    static constexpr uint32_t kBankFrameCount = 43;
    // 外置姿态库为大角度摆头补足下方画布；原始素材与小幅回退缓存仍保持 350 高。
    static constexpr uint32_t kBankHeight = 400;
    static constexpr uint32_t kBankLowerHeight = kBankHeight - kOpaqueHeight;
    static constexpr uint32_t kBankHeaderSize = 64;
    static constexpr uint32_t kBankEntrySize = 24;
    static constexpr uint32_t kRawBytes =
        ANIME_BADGE_HEAD_WIDTH * (kOpaqueHeight * 2 + kBankLowerHeight * 3);
    std::array<Frame, 13> frames_{};
    std::array<BankEntry, kBankFrameCount> bank_entries_{};
    size_t frame_count_ = 0;
    size_t allocated_bytes_ = 0;
    size_t active_frame_ = 0;
    size_t bank_count_ = 0;
    const uint8_t* bank_data_ = nullptr;
    uint64_t decode_total_us_ = 0;
    uint32_t decode_count_ = 0, decode_max_us_ = 0;

    static void DropFrameCache(Frame& frame) {
        // 描述符和像素地址会循环复用，写入前清除旧图像及图像头缓存。
        if (frame.image.data)
            lv_image_cache_drop(&frame.image);
        if (frame.draw_buf.data)
            lv_image_cache_drop(&frame.draw_buf);
        if (frame.lower_image.data)
            lv_image_cache_drop(&frame.lower_image);
        if (frame.lower_buf.data)
            lv_image_cache_drop(&frame.lower_buf);
    }

    static uint32_t ReadU32(const uint8_t* data) {
        // assets 文件带索引前缀，映射地址不一定四字节对齐，逐字节读取小端整数。
        return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 |
               static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
    }

    bool InitializeBank() {
#if LV_USE_LZ4_INTERNAL
        void* mapped = nullptr;
        size_t size = 0;
        if (!Assets::GetInstance().GetAssetData("badge_pose_bank.bin", mapped, size))
            return false;
        const auto data = static_cast<const uint8_t*>(mapped);
        if (!data || size < kBankHeaderSize || size > std::numeric_limits<uint32_t>::max() ||
            memcmp(data, "BDGPOSE1", 8) || ReadU32(data + 8) != 1 ||
            ReadU32(data + 12) != kBankHeaderSize || ReadU32(data + 16) != kBankFrameCount ||
            ReadU32(data + 20) != ANIME_BADGE_HEAD_WIDTH || ReadU32(data + 24) != kBankHeight ||
            ReadU32(data + 28) != kOpaqueHeight || ReadU32(data + 32) != kRawBytes ||
            ReadU32(data + 36) != 233 || ReadU32(data + 40) != 320 ||
            ReadU32(data + 44) != kBankHeaderSize || ReadU32(data + 48) != kBankEntrySize ||
            ReadU32(data + 56) != 1 || ReadU32(data + 60) != 0) {
            ESP_LOGW("AnimeBadge", "姿态库文件头无效，使用自然姿态缓存");
            return false;
        }
        const uint32_t data_offset = ReadU32(data + 52);
        const uint32_t table_end = kBankHeaderSize + kBankFrameCount * kBankEntrySize;
        if (size < table_end || data_offset < table_end || data_offset > size || data_offset % 16) {
            ESP_LOGW("AnimeBadge", "姿态库目录边界无效，使用自然姿态缓存");
            return false;
        }
        uint32_t previous_end = data_offset;
        for (size_t i = 0; i < kBankFrameCount; ++i) {
            const auto entry = data + kBankHeaderSize + i * kBankEntrySize;
            const int expected_angle = -100 + static_cast<int>(i) * 10;
            const uint32_t offset = ReadU32(entry + 4);
            const uint32_t length = ReadU32(entry + 8);
            if (ReadU32(entry) != static_cast<uint32_t>(expected_angle) || offset < previous_end ||
                offset > size || !length || length > size - offset ||
                length > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
                ReadU32(entry + 12) != kRawBytes || ReadU32(entry + 20) != 0) {
                ESP_LOGW("AnimeBadge", "姿态库第 %u 项无效，使用自然姿态缓存",
                         static_cast<unsigned>(i));
                return false;
            }
            bank_entries_[i] = {expected_angle, offset, length, ReadU32(entry + 16)};
            previous_end = offset + length;
        }
        if (!AllocateFrames(2, kBankLowerHeight)) {
            ESP_LOGW("AnimeBadge", "姿态库双缓冲分配失败，使用自然姿态缓存");
            return false;
        }
        // 先验证每个角度一次，防止损坏资源在运行中替换仍被界面引用的缓冲。
        // 这里复用同一块内存，之后切档只解压，不反复扫描整幅图像的 CRC。
        const int64_t start = esp_timer_get_time();
        for (const auto& entry : bank_entries_) {
            const int decoded = LZ4_decompress_safe(
                reinterpret_cast<const char*>(data + entry.offset),
                reinterpret_cast<char*>(frames_[0].pixels), entry.size, kRawBytes);
            if (decoded != static_cast<int>(kRawBytes) ||
                esp_rom_crc32_le(0, frames_[0].pixels, kRawBytes) != entry.crc) {
                ESP_LOGW("AnimeBadge", "姿态库解压或 CRC 无效，使用自然姿态缓存");
                Reset();
                return false;
            }
            vTaskDelay(1);
        }
        for (auto& frame : frames_) {
            if (!frame.pixels)
                continue;
            lv_draw_buf_to_image(&frame.draw_buf, &frame.image);
            lv_draw_buf_to_image(&frame.lower_buf, &frame.lower_image);
            frame.angle = std::numeric_limits<int>::min();
        }
        bank_data_ = data;
        bank_count_ = kBankFrameCount;
        // 验证后先发布自然零度，两个描述符地址轮换使 LVGL 正确识别每次换帧。
        SelectBankFrame(0);
        if (frames_[active_frame_].angle != 0 ||
            heap_caps_get_free_size(kMemoryCaps) < kReservedBytes) {
            Reset();
            return false;
        }
        decode_total_us_ = decode_count_ = decode_max_us_ = 0;
        ESP_LOGI("AnimeBadge", "外置姿态库：%u 档，双缓冲 %u 字节，校验 %ums，PSRAM 剩余 %u",
                 static_cast<unsigned>(bank_count_), static_cast<unsigned>(allocated_bytes_),
                 static_cast<unsigned>((esp_timer_get_time() - start) / 1000),
                 static_cast<unsigned>(heap_caps_get_free_size(kMemoryCaps)));
        return true;
#else
        return false;
#endif
    }

    void SelectBankFrame(int angle) {
#if LV_USE_LZ4_INTERNAL
        angle = angle < -100 ? -100 : (angle > 320 ? 320 : angle);
        const size_t index = static_cast<size_t>(angle + 100 + 5) / 10;
        const auto& entry = bank_entries_[index];
        for (size_t i = 0; i < frame_count_; ++i) {
            if (frames_[i].angle == entry.angle) {
                active_frame_ = i;
                return;
            }
        }
        const size_t next = active_frame_ ^ 1;
        auto& frame = frames_[next];
        DropFrameCache(frame);
        const int64_t start = esp_timer_get_time();
        const int decoded =
            LZ4_decompress_safe(reinterpret_cast<const char*>(bank_data_ + entry.offset),
                                reinterpret_cast<char*>(frame.pixels), entry.size, kRawBytes);
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - start);
        if (decoded != static_cast<int>(kRawBytes)) {
            // 已通过启动校验的库若运行时读取失败，保留当前完整帧，不发布半幅图像。
            frame.angle = std::numeric_limits<int>::min();
            ESP_LOGE("AnimeBadge", "姿态解压失败，保留上一帧");
            return;
        }
        decode_total_us_ += elapsed;
        ++decode_count_;
        if (elapsed > decode_max_us_)
            decode_max_us_ = elapsed;
        lv_draw_buf_flush_cache(&frame.draw_buf, nullptr);
        lv_draw_buf_flush_cache(&frame.lower_buf, nullptr);
        frame.angle = entry.angle;
        active_frame_ = next;
#else
        (void)angle;
#endif
    }

    size_t NearestFrame(int angle) const {
        angle = angle < -30 ? -30 : (angle > 30 ? 30 : angle);
        size_t nearest = 0;
        int nearest_distance = 1000;
        for (size_t i = 0; i < frame_count_; ++i) {
            int distance = angle - frames_[i].angle;
            if (distance < 0)
                distance = -distance;
            if (distance < nearest_distance) {
                nearest = i;
                nearest_distance = distance;
            }
        }
        return nearest;
    }

    struct ScratchCanvas {
        uint8_t* pixels = nullptr;
        lv_obj_t* screen = nullptr;
        lv_obj_t* canvas = nullptr;
        lv_draw_buf_t draw_buf = {};

        ~ScratchCanvas() { Reset(); }

        bool Initialize() {
            const uint32_t stride =
                lv_draw_buf_width_to_stride(ANIME_BADGE_HEAD_WIDTH, LV_COLOR_FORMAT_ARGB8888);
            const size_t pixel_bytes = static_cast<size_t>(stride) * ANIME_BADGE_HEAD_HEIGHT;
            const size_t allocation_bytes =
                (pixel_bytes + kAlignment - 1) / kAlignment * kAlignment;
            if (heap_caps_get_free_size(kMemoryCaps) < allocation_bytes + kReservedBytes)
                return false;
            pixels = static_cast<uint8_t*>(
                heap_caps_aligned_alloc(kAlignment, allocation_bytes, kMemoryCaps));
            if (!pixels ||
                lv_draw_buf_init(&draw_buf, ANIME_BADGE_HEAD_WIDTH, ANIME_BADGE_HEAD_HEIGHT,
                                 LV_COLOR_FORMAT_ARGB8888, stride, pixels,
                                 static_cast<uint32_t>(allocation_bytes)) != LV_RESULT_OK)
                return false;
            lv_draw_buf_set_flag(&draw_buf, LV_IMAGE_FLAGS_MODIFIABLE);
            // 独立屏幕从未加载，临时画布不会覆盖正在展示的徽章。
            screen = lv_obj_create(nullptr);
            if (!screen)
                return false;
            canvas = lv_canvas_create(screen);
            if (!canvas)
                return false;
            lv_canvas_set_draw_buf(canvas, &draw_buf);
            return true;
        }

        void Reset() {
            if (screen)
                lv_obj_delete(screen);
            screen = canvas = nullptr;
            if (pixels) {
                lv_image_cache_drop(&draw_buf);
                heap_caps_free(pixels);
            }
            pixels = nullptr;
            draw_buf = {};
        }
    };

    static void ConvertFrame(const lv_draw_buf_t& source, lv_draw_buf_t& upper,
                             lv_draw_buf_t& lower) {
        const uint32_t alpha_stride = lower.header.stride / 2;
        auto alpha = lower.data + lower.header.stride * lower.header.h;
        for (uint32_t y = 0; y < source.header.h; ++y) {
            const auto row =
                reinterpret_cast<const lv_color32_t*>(source.data + y * source.header.stride);
            const bool opaque = y < kOpaqueHeight;
            const uint32_t local_y = opaque ? y : y - kOpaqueHeight;
            auto& destination = opaque ? upper : lower;
            auto color_row = destination.data + local_y * destination.header.stride;
            for (uint32_t x = 0; x < source.header.w; ++x) {
                // 使用 LVGL 的通道字段读取 ARGB，避免假定源像素的内存字节序。
                const auto color = row[x];
                uint16_t rgb565 = static_cast<uint16_t>(
                    (color.red >> 3) << 11 | (color.green >> 2) << 5 | (color.blue >> 3));
                color_row[x * 2] = rgb565 & 0xff;
                color_row[x * 2 + 1] = rgb565 >> 8;
                // 完全透明保持零，仅把接近不透明的舍入误差归一为快速不透明路径。
                if (!opaque)
                    alpha[local_y * alpha_stride + x] = color.alpha >= 253 ? 255 : color.alpha;
            }
        }
        lv_draw_buf_flush_cache(&upper, nullptr);
        lv_draw_buf_flush_cache(&lower, nullptr);
    }

    bool AllocateFrames(size_t count, uint32_t lower_height = kLowerHeight) {
        Reset();
        const uint32_t stride =
            lv_draw_buf_width_to_stride(ANIME_BADGE_HEAD_WIDTH, LV_COLOR_FORMAT_RGB565);
        const size_t upper_bytes = static_cast<size_t>(stride) * kOpaqueHeight;
        const size_t lower_bytes = static_cast<size_t>(stride + stride / 2) * lower_height;
        const size_t pixel_bytes = upper_bytes + lower_bytes;
        const size_t allocation_bytes = (pixel_bytes + kAlignment - 1) / kAlignment * kAlignment;
        if (heap_caps_get_free_size(kMemoryCaps) < count * allocation_bytes + kReservedBytes)
            return false;

        for (size_t i = 0; i < count; ++i) {
            // 分档分配避免依赖一整块连续内存，并始终保留音频和联网工作空间。
            if (heap_caps_get_free_size(kMemoryCaps) < allocation_bytes + kReservedBytes ||
                heap_caps_get_largest_free_block(kMemoryCaps) < allocation_bytes) {
                Reset();
                return false;
            }
            auto& frame = frames_[i];
            frame.pixels = static_cast<uint8_t*>(
                heap_caps_aligned_alloc(kAlignment, allocation_bytes, kMemoryCaps));
            if (!frame.pixels ||
                lv_draw_buf_init(&frame.draw_buf, ANIME_BADGE_HEAD_WIDTH, kOpaqueHeight,
                                 LV_COLOR_FORMAT_RGB565, stride, frame.pixels,
                                 static_cast<uint32_t>(upper_bytes)) != LV_RESULT_OK ||
                lv_draw_buf_init(&frame.lower_buf, ANIME_BADGE_HEAD_WIDTH, lower_height,
                                 LV_COLOR_FORMAT_RGB565A8, stride, frame.pixels + upper_bytes,
                                 static_cast<uint32_t>(lower_bytes)) != LV_RESULT_OK) {
                Reset();
                return false;
            }
            lv_draw_buf_set_flag(&frame.draw_buf, LV_IMAGE_FLAGS_MODIFIABLE);
            lv_draw_buf_set_flag(&frame.lower_buf, LV_IMAGE_FLAGS_MODIFIABLE);
            frame.angle = (static_cast<int>(i) - static_cast<int>(count / 2)) * 60 /
                          static_cast<int>(count - 1);
            allocated_bytes_ += allocation_bytes;
        }
        frame_count_ = count;
        return true;
    }
};

#endif
