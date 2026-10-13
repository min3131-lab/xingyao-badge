#pragma once

#include "sdkconfig.h"

#if CONFIG_ANIME_BADGE_DESKTOP
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "badge_companion.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>

// 复用 SensorLib 的 QMI8658 初始化、寄存器布局及量程换算，裁减为徽章所需的六轴读取。
// 来源：https://github.com/lewisxhe/SensorLib/blob/master/src/SensorQMI8658.hpp
// 本仓库外层微雪官方示例也附带同一 SensorLib；没有复制 FIFO、计步等无关功能。
/*
MIT License
Copyright (c) 2022 lewis he

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

class BadgeMotionSensor {
public:
    using MotionCallback = std::function<void(float, float, float)>;

    // 板对象与此传感器任务在整次开机期间常驻；回调仅发布数值，不操作 LVGL。
    bool Start(i2c_master_bus_handle_t bus, MotionCallback callback) {
        if (device_ || !callback) {
            return false;
        }
        // 1.75C 官方示例使用 0x6B，与触摸和音频共享 GPIO15/14 的 I2C 总线。
        if (i2c_master_probe(bus, kAddress, kTimeoutMs) != ESP_OK) {
            ESP_LOGW(kTag, "未检测到 QMI8658，保留角色待机动画");
            return false;
        }
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = kAddress;
        config.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
            return false;
        }
        if (!Initialize()) {
            ESP_LOGW(kTag, "QMI8658 初始化失败，保留角色待机动画");
            i2c_master_bus_rm_device(device_);
            device_ = nullptr;
            return false;
        }
        callback_ = std::move(callback);
        if (xTaskCreate([](void* arg) { static_cast<BadgeMotionSensor*>(arg)->Run(); },
                        "badge_motion", 4096, this, 2, &task_) != pdPASS) {
            Write(kCtrl7, 0);
            i2c_master_bus_rm_device(device_);
            device_ = nullptr;
            callback_ = nullptr;
            ESP_LOGW(kTag, "六轴任务创建失败，保留角色待机动画");
            return false;
        }
        ESP_LOGI(kTag, "QMI8658 已启用：50Hz 采样，六轴跟随，I2C 地址 0x6B");
        return true;
    }

    uint32_t steps() const { return steps_.load(); }

    void SetPaused(bool paused) { paused_.store(paused, std::memory_order_relaxed); }

private:
    static constexpr const char* kTag = "BadgeMotion";
    static constexpr uint8_t kAddress = 0x6B;
    static constexpr int kTimeoutMs = 20;
    static constexpr uint8_t kWhoAmI = 0x00;
    static constexpr uint8_t kCtrl1 = 0x02;
    static constexpr uint8_t kCtrl2 = 0x03;
    static constexpr uint8_t kCtrl3 = 0x04;
    static constexpr uint8_t kCtrl5 = 0x06;
    static constexpr uint8_t kCtrl7 = 0x08;
    static constexpr uint8_t kStatus0 = 0x2E;
    static constexpr uint8_t kAccelX = 0x35;
    static constexpr uint8_t kResetResult = 0x4D;
    static constexpr uint8_t kReset = 0x60;
    i2c_master_dev_handle_t device_ = nullptr;
    TaskHandle_t task_ = nullptr;
    MotionCallback callback_;
    BadgeStepDetector step_detector_;
    std::atomic<uint32_t> steps_{0};
    std::atomic<bool> paused_{false};

    bool Write(uint8_t reg, uint8_t value) {
        const uint8_t buffer[] = {reg, value};
        return i2c_master_transmit(device_, buffer, sizeof(buffer), kTimeoutMs) == ESP_OK;
    }

    bool Read(uint8_t reg, uint8_t* output, size_t size) {
        // ESP-IDF 主机驱动在共享总线上串行执行完整事务；失败只丢弃此次样本。
        return i2c_master_transmit_receive(device_, &reg, 1, output, size, kTimeoutMs) == ESP_OK;
    }

    bool Initialize() {
        uint8_t value = 0;
        if (!Read(kWhoAmI, &value, 1) || value != 0x05 || !Write(kReset, 0xB0)) {
            return false;
        }
        bool reset_ready = false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (Read(kResetResult, &value, 1) && value == 0x80) {
                reset_ready = true;
                break;
            }
        }
        // 0x40 保留小端数据并开启地址递增，后续一次读取连续 12 字节六轴数据。
        // 加速度 ±4g / 125Hz，角速度 ±512°/s / 112.1Hz；六轴模式由陀螺仪决定 ODR。
        // CTRL5=0x77 开启两组 LPF_MODE_3，截止约 ODR 的 13.37%。
        if (!reset_ready || !Write(kCtrl1, 0x40) || !Write(kCtrl7, 0x00) || !Write(kCtrl2, 0x16) ||
            !Write(kCtrl3, 0x56) || !Write(kCtrl5, 0x77) || !Write(kCtrl7, 0x03)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        uint8_t setup[7] = {};
        if (!Read(kCtrl1, setup, sizeof(setup))) {
            return false;
        }
        ESP_LOGI(kTag, "配置回读 CTRL1=%02X CTRL2=%02X CTRL3=%02X CTRL5=%02X CTRL7=%02X", setup[0],
                 setup[1], setup[2], setup[4], setup[6]);
        return setup[0] == 0x40 && setup[1] == 0x16 && setup[2] == 0x56 && setup[4] == 0x77 &&
               setup[6] == 0x03;
    }

    static float Length(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

    void Run() {
        float gravity[3] = {};
        float neutral[3] = {};
        float calibration_sum[3] = {};
        float shake = 0;
        bool filter_ready = false;
        bool hardware_paused = false;
        unsigned errors = 0;
        unsigned samples = 0;
        unsigned raw_samples = 0;
        unsigned calibration_count = 0;
        unsigned settle_samples = 10;
        int64_t last_sample = esp_timer_get_time();
        TickType_t wake = xTaskGetTickCount();
        auto reset_filter = [&]() {
            filter_ready = false;
            calibration_count = 0;
            settle_samples = 10;
            for (float& component : calibration_sum) {
                component = 0;
            }
            shake = 0;
            callback_(0, 0, 0);
        };
        while (true) {
            const bool pause = paused_.load(std::memory_order_relaxed);
            if (pause != hardware_paused) {
                if (Write(kCtrl7, pause ? 0x00 : 0x03)) {
                    hardware_paused = pause;
                    reset_filter();
                }
            }
            if (pause || hardware_paused) {
                // 息屏关闭两组测量，任务低频等待；亮屏后以当前佩戴角度重新归零。
                vTaskDelay(pdMS_TO_TICKS(100));
                wake = xTaskGetTickCount();
                continue;
            }
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
            uint8_t status = 0;
            uint8_t buffer[12] = {};
            if (!Read(kStatus0, &status, 1) || (status & 0x03) != 0x03 ||
                !Read(kAccelX, buffer, sizeof(buffer))) {
                if (++errors == 25) {
                    ESP_LOGW(kTag, "六轴读取暂不可用，退回待机动作");
                    reset_filter();
                }
                continue;
            }
            errors = 0;
            float accel[3];
            float gyro[3];
            for (int axis = 0; axis < 3; ++axis) {
                auto unpack = [&buffer](int index) {
                    return static_cast<int16_t>(static_cast<uint16_t>(buffer[index]) |
                                                (static_cast<uint16_t>(buffer[index + 1]) << 8));
                };
                accel[axis] = unpack(axis * 2) * (4.0f / 32768.0f);
                gyro[axis] = unpack(6 + axis * 2) * (512.0f / 32768.0f);
            }
            const int64_t now = esp_timer_get_time();
            const float dt = std::clamp((now - last_sample) / 1000000.0f, 0.001f, 0.05f);
            last_sample = now;
            const float magnitude = Length(accel[0], accel[1], accel[2]);
            const float speed = Length(gyro[0], gyro[1], gyro[2]);
            steps_.fetch_add(step_detector_.Sample(magnitude, static_cast<uint32_t>(now / 1000)));
            if (++raw_samples % 250 == 0 && !filter_ready) {
                ESP_LOGI(kTag,
                         "等待稳定姿态 samples=%u accel=[%.3f,%.3f,%.3f]g "
                         "gyro=[%.1f,%.1f,%.1f]dps",
                         calibration_count, accel[0], accel[1], accel[2], gyro[0], gyro[1],
                         gyro[2]);
            }
            if (magnitude < 0.1f) {
                continue;
            }
            if (!filter_ready) {
                // 开机和亮屏先丢弃 0.2 秒数据，再用 20 个稳定样本均值归零。
                // 避免将传感器启动瞬态或按键动作锁定成长期偏移基准。
                if (settle_samples > 0) {
                    --settle_samples;
                    continue;
                }
                if (magnitude < 0.65f || magnitude > 1.35f || speed > 45.0f) {
                    calibration_count = 0;
                    for (float& component : calibration_sum) {
                        component = 0;
                    }
                    continue;
                }
                for (int axis = 0; axis < 3; ++axis) {
                    calibration_sum[axis] += accel[axis];
                }
                if (++calibration_count < 20) {
                    continue;
                }
                const float sum_length =
                    Length(calibration_sum[0], calibration_sum[1], calibration_sum[2]);
                for (int axis = 0; axis < 3; ++axis) {
                    gravity[axis] = neutral[axis] =
                        calibration_sum[axis] / std::max(sum_length, 0.01f);
                }
                filter_ready = true;
                callback_(0, 0, 0);
                ESP_LOGI(kTag, "姿态归零完成 neutral=[%.3f,%.3f,%.3f] accel=[%.3f,%.3f,%.3f]g",
                         neutral[0], neutral[1], neutral[2], accel[0], accel[1], accel[2]);
                continue;
            }
            float rotation[3];
            for (int axis = 0; axis < 3; ++axis) {
                // 静止的小偏置不推动角色；角速度转为弧度后参与重力方向预测。
                rotation[axis] = std::abs(gyro[axis]) < 1.2f ? 0 : gyro[axis] * 0.017453293f;
            }
            const float predicted[3] = {
                gravity[0] + (gravity[1] * rotation[2] - gravity[2] * rotation[1]) * dt,
                gravity[1] + (gravity[2] * rotation[0] - gravity[0] * rotation[2]) * dt,
                gravity[2] + (gravity[0] * rotation[1] - gravity[1] * rotation[0]) * dt,
            };
            // 急晃时降低加速度权重，避免把平移加速度误当作持续倾斜。
            const float correction = std::abs(magnitude - 1.0f) < 0.18f ? 0.14f : 0.025f;
            for (int axis = 0; axis < 3; ++axis) {
                gravity[axis] =
                    predicted[axis] * (1 - correction) + accel[axis] / magnitude * correction;
            }
            const float gravity_length = Length(gravity[0], gravity[1], gravity[2]);
            for (float& component : gravity) {
                component /= std::max(gravity_length, 0.01f);
            }
            const float acceleration =
                Length(accel[0] - gravity[0], accel[1] - gravity[1], accel[2] - gravity[2]);
            const float impulse = std::clamp(
                std::max((acceleration - 0.10f) * 1.5f, (speed - 35.0f) / 300.0f), 0.0f, 1.0f);
            shake = std::max(impulse, shake * 0.88f);
            // 轴映射沿用微雪 04_Immersive_block：屏幕 X←加速度 Y，屏幕 Y←加速度 X。
            const float x = std::clamp((gravity[1] - neutral[1]) * 1.7f, -1.0f, 1.0f);
            const float y = std::clamp((gravity[0] - neutral[0]) * 1.7f, -1.0f, 1.0f);
            callback_(x, y, shake);
            if (++samples == 25 || samples % 250 == 0) {
                ESP_LOGI(kTag, "跟随采样 x=%.2f y=%.2f shake=%.2f", x, y, shake);
                ESP_LOGI(kTag,
                         "六轴诊断 accel=[%.3f,%.3f,%.3f]g gyro=[%.1f,%.1f,%.1f]dps "
                         "gravity=[%.3f,%.3f,%.3f] neutral=[%.3f,%.3f,%.3f]",
                         accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2], gravity[0],
                         gravity[1], gravity[2], neutral[0], neutral[1], neutral[2]);
            }
        }
    }
};
#endif
