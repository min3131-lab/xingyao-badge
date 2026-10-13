#pragma once
#include <lvgl.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include "badge_companion.h"

// 使用 LVGL 原生控件和内置符号，不再解码整屏壁纸；应用共用一个按需重建的容器。
class AnimeBadgeDesktop {
public:
    enum class Item { Chat, Settings, FeedHappy, Save, RhythmStart, RhythmStop, Activity, Badge };
    enum class View { Launcher, Feed, Stats, Card, Walk, Focus, Rhythm };
    BadgeCompanion model;
    lv_obj_t* Create(lv_obj_t* parent, const void*, size_t, std::function<void(Item)> cb) {
        on_select_ = std::move(cb);
        root_ = lv_obj_create(parent);
        lv_obj_set_size(root_, 466, 466);
        lv_obj_set_pos(root_, 0, 0);
        lv_obj_set_style_pad_all(root_, 0, 0);
        lv_obj_set_style_border_width(root_, 0, 0);
        lv_obj_set_style_radius(root_, 0, 0);
        lv_obj_set_style_bg_color(root_, C(0x020609), 0);
        lv_obj_set_style_text_color(root_, C(0xe6f0f4), 0);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(
            root_,
            [](lv_event_t* e) {
                auto self = static_cast<AnimeBadgeDesktop*>(lv_event_get_user_data(e));
                auto input = lv_indev_active();
                if (!input)
                    return;
                auto direction = lv_indev_get_gesture_dir(input);
                if (self->view_ == View::Launcher &&
                    (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT)) {
                    self->launcher_page_ = 1 - self->launcher_page_;
                    self->Build();
                } else if (self->view_ != View::Launcher && direction == LV_DIR_RIGHT)
                    self->Back();
                else if (direction == LV_DIR_BOTTOM)
                    self->Emit(Item::Badge);
                lv_indev_wait_release(input);
            },
            LV_EVENT_GESTURE, this);
        Build();
        return root_;
    }
    void Launcher() {
        StopRhythm();
        view_ = View::Launcher;
        Build();
    }
    bool Back() {
        if (view_ == View::Launcher)
            return false;
        Launcher();
        return true;
    }
    void Hide() { StopRhythm(); }
    View view() const { return view_; }
    void SetBattery(int percent, bool charging) {
        battery_ = percent;
        charging_ = charging;
        Update();
    }
    void Update() {
        char text[128];
        auto& s = model.state;
        switch (view_) {
            case View::Launcher:
                if (battery_ >= 0)
                    std::snprintf(text, sizeof(text), "%s %d%%", charging_ ? "充电中" : "电量",
                                  battery_);
                else
                    std::snprintf(text, sizeof(text), "%lu 金币",
                                  static_cast<unsigned long>(s.coins));
                Set(info_[0], text);
                break;
            case View::Feed:
                std::snprintf(text, sizeof(text), "%lu 金币  ·  饱腹 %lu", (unsigned long)s.coins,
                              (unsigned long)s.fullness);
                Set(info_[0], text);
                break;
            case View::Stats:
                if (!tab_) {
                    std::snprintf(text, sizeof(text), "心情  %s", model.Mood());
                    Set(info_[0], text);
                    std::snprintf(text, sizeof(text), "精力  %lu / 100", (unsigned long)s.energy);
                    Set(info_[1], text);
                    std::snprintf(text, sizeof(text), "饱腹  %lu / 100", (unsigned long)s.fullness);
                    Set(info_[2], text);
                    Set(info_[3], model.reaction);
                    lv_bar_set_value(gauges_[0], s.energy, LV_ANIM_OFF);
                    lv_bar_set_value(gauges_[1], s.fullness, LV_ANIM_OFF);
                } else {
                    std::snprintf(text, sizeof(text), "陪伴第 %lu 天   ·   Lv.%lu",
                                  (unsigned long)(s.seconds / 86400 + 1),
                                  (unsigned long)model.Level());
                    Set(info_[0], text);
                    std::snprintf(text, sizeof(text), "亲密度  %lu", (unsigned long)s.affinity);
                    Set(info_[1], text);
                    std::snprintf(text, sizeof(text), "一起走过  %lu 步", (unsigned long)s.steps);
                    Set(info_[2], text);
                    std::snprintf(text, sizeof(text), "一起专注  %lu 分钟",
                                  (unsigned long)s.focus_minutes);
                    Set(info_[3], text);
                    lv_bar_set_value(gauges_[0], s.affinity % 20 * 5, LV_ANIM_OFF);
                }
                break;
            case View::Walk:
                std::snprintf(text, sizeof(text), "%lu",
                              (unsigned long)(tab_ ? model.outing_steps : s.day_steps));
                Set(info_[0], text);
                if (tab_)
                    std::snprintf(text, sizeof(text), "同行 %02lu:%02lu",
                                  (unsigned long)(model.outing_seconds / 60),
                                  (unsigned long)(model.outing_seconds % 60));
                else
                    std::snprintf(text, sizeof(text), "步  ·  活跃 %lu 分钟",
                                  (unsigned long)(s.active_seconds / 60));
                Set(info_[1], text);
                if (gauges_[0])
                    lv_bar_set_value(gauges_[0], std::min<uint32_t>(100u, s.day_steps / 20),
                                     LV_ANIM_OFF);
                break;
            case View::Focus:
                if (!tab_) {
                    auto left = model.focus_left ? model.focus_left : model.focus_length * 60;
                    std::snprintf(text, sizeof(text), "%02lu:%02lu", (unsigned long)(left / 60),
                                  (unsigned long)(left % 60));
                    Set(info_[0], text);
                    Set(info_[1], model.focus_running ? "暂停"
                                  : model.focus_left  ? "继续"
                                                      : "开始");
                    std::snprintf(text, sizeof(text), "完成奖励 %lu 金币",
                                  (unsigned long)(model.focus_length / 5 * 2));
                    Set(info_[2], text);
                }
                break;
            default:
                break;
        }
    }
    void Meter(float level) {
        if (view_ != View::Rhythm)
            return;
        // 柱形图展示真实音量的最近历史；不将时域能量冒充频谱或分贝测量。
        for (size_t i = 0; i < history_.size() - 1; ++i)
            history_[i] = history_[i + 1];
        history_.back() = std::clamp(level, 0.0f, 1.0f);
        for (size_t i = 0; i < bars_.size(); ++i) {
            int h = 4 + history_[i] * 112;
            lv_obj_set_height(bars_[i], h);
            lv_obj_set_y(bars_[i], 283 - h);
        }
    }
    void Open(View view) {
        StopRhythm();
        view_ = view;
        tab_ = 0;
        Build();
        if (view == View::Rhythm)
            Emit(Item::RhythmStart);
    }

private:
    lv_obj_t* root_ = nullptr;
    std::array<lv_obj_t*, 6> info_{};
    std::array<lv_obj_t*, 2> gauges_{};
    std::array<lv_obj_t*, 16> bars_{};
    std::array<float, 16> history_{};
    std::function<void(Item)> on_select_;
    View view_ = View::Launcher;
    int launcher_page_ = 0, tab_ = 0, battery_ = -1;
    bool charging_ = false;
    static lv_color_t C(uint32_t c) { return lv_color_hex(c); }
    void Emit(Item i) {
        if (on_select_)
            on_select_(i);
    }
    void StopRhythm() {
        if (view_ == View::Rhythm)
            Emit(Item::RhythmStop);
    }
    static void Set(lv_obj_t* o, const char* text) {
        if (o && std::strcmp(lv_label_get_text(o), text))
            lv_label_set_text(o, text);
    }
    lv_obj_t* Text(const char* text, int x, int y, int width = 336, uint32_t color = 0xe6f0f4) {
        auto o = lv_label_create(root_);
        lv_label_set_text(o, text);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_width(o, width);
        lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(o, C(color), 0);
        return o;
    }
    void Bind(lv_obj_t* o, int id) {
        lv_obj_set_user_data(o, reinterpret_cast<void*>(static_cast<intptr_t>(id)));
        lv_obj_add_event_cb(
            o,
            [](lv_event_t* e) {
                auto self = static_cast<AnimeBadgeDesktop*>(lv_event_get_user_data(e));
                auto target = static_cast<lv_obj_t*>(lv_event_get_target(e));
                int id = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                self->Handle(id);
            },
            LV_EVENT_SHORT_CLICKED, this);
    }
    lv_obj_t* Button(const char* text, int x, int y, int w, int h, int id, bool accent = false) {
        auto o = lv_button_create(root_);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, w, h);
        lv_obj_set_style_radius(o, 18, 0);
        lv_obj_set_style_shadow_width(o, 0, 0);
        lv_obj_set_style_bg_color(o, C(accent ? 0x153b42 : 0x0d171d), 0);
        lv_obj_set_style_border_color(o, C(accent ? 0x376b73 : 0x21313a), 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_text_color(o, C(accent ? 0x6ee5e7 : 0xe6f0f4), 0);
        auto label = lv_label_create(o);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        Bind(o, id);
        return label;
    }
    lv_obj_t* Gauge(int y) {
        auto o = lv_bar_create(root_);
        lv_obj_set_pos(o, 88, y);
        lv_obj_set_size(o, 290, 7);
        lv_obj_set_style_bg_color(o, C(0x17262e), LV_PART_MAIN);
        lv_obj_set_style_bg_color(o, C(0x6ee5e7), LV_PART_INDICATOR);
        return o;
    }
    void Tabs(const char* a, const char* b) {
        Button(a, 80, 98, 150, 38, 30, !tab_);
        Button(b, 236, 98, 150, 38, 31, tab_);
    }
    void Number(lv_obj_t* o) { lv_obj_set_style_text_font(o, &lv_font_montserrat_40, 0); }
    void Build() {
        if (!root_)
            return;
        lv_obj_clean(root_);
        info_.fill(nullptr);
        gauges_.fill(nullptr);
        bars_.fill(nullptr);
        if (view_ == View::Launcher) {
            info_[0] = Text("", 133, 52, 200, 0x98a9b4);
            const char* names[] = {"投喂", "属性", "对话", "名片", "同行", "专注", "声律", "设置"};
            const char* icons[] = {LV_SYMBOL_TINT,       LV_SYMBOL_EYE_OPEN, LV_SYMBOL_CALL,
                                   LV_SYMBOL_IMAGE,      LV_SYMBOL_LOOP,     LV_SYMBOL_BELL,
                                   LV_SYMBOL_VOLUME_MAX, LV_SYMBOL_SETTINGS};
            const uint32_t colors[] = {0xf1bc80, 0xd7b2ff, 0x6ee5e7, 0x9bbcff,
                                       0x8fe0bb, 0xe6cf84, 0x80c8f1, 0xb4c1cf};
            for (int i = 0; i < 4; ++i) {
                int index = launcher_page_ * 4 + i;
                int x = i % 2 ? 257 : 113, y = i / 2 ? 249 : 112;
                auto label = Button(icons[index], x, y, 96, 86, index + 1);
                Number(label);
                lv_obj_set_style_text_color(label, C(colors[index]), 0);
                Text(names[index], x - 12, y + 94, 120);
            }
            auto page_label = Button("", 179, 393, 108, 32, 20);
            auto pager = lv_obj_get_parent(page_label);
            lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(pager, 0, 0);
            for (int i = 0; i < 2; ++i) {
                auto dot = lv_obj_create(pager);
                lv_obj_set_size(dot, i == launcher_page_ ? 20 : 7, 7);
                lv_obj_set_style_radius(dot, 4, 0);
                lv_obj_set_style_border_width(dot, 0, 0);
                lv_obj_set_style_bg_color(dot, C(i == launcher_page_ ? 0x6ee5e7 : 0x314550), 0);
                lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_align(dot, LV_ALIGN_CENTER, i ? 16 : -16, 0);
            }
            Update();
            return;
        }
        const char* title = view_ == View::Feed    ? "投喂"
                            : view_ == View::Stats ? "星瑶"
                            : view_ == View::Card  ? "名片"
                            : view_ == View::Walk  ? "同行"
                            : view_ == View::Focus ? "专注"
                                                   : "声律";
        Text(title, 103, 56, 260);
        Button("返回", 177, 386, 112, 40, 0);
        if (view_ == View::Feed) {
            info_[0] = Text("", 65, 101, 336, 0xe6cf84);
            Button("鲜果   ·   5 金币", 83, 148, 300, 51, 40);
            Button("饼干   ·   8 金币", 83, 208, 300, 51, 41);
            Button("热饮   ·   6 金币", 83, 268, 300, 51, 42);
            info_[1] = Text("摸摸 · 同行 · 专注赚金币", 65, 337, 336, 0x98a9b4);
        } else if (view_ == View::Stats) {
            Tabs("此刻", "与你");
            info_[0] = Text("", 65, 157, 336, 0x6ee5e7);
            info_[1] = Text("", 65, 207);
            gauges_[0] = Gauge(243);
            info_[2] = Text("", 65, 268);
            if (!tab_)
                gauges_[1] = Gauge(304);
            info_[3] = Text("", 68, 334, 330, 0x98a9b4);
        } else if (view_ == View::Card) {
            Text("星瑶的同行者", 65, 110, 336, 0x6ee5e7);
            auto qr = lv_qrcode_create(root_);
            lv_qrcode_set_size(qr, 168);
            lv_qrcode_set_dark_color(qr, C(0x071016));
            lv_qrcode_set_light_color(qr, C(0xe6f0f4));
            // 离线文字名片，无虚构社交账号；二维码完全在设备本地生成。
            static const char card[] = "星瑶的同行者\n把星光带在身边\n电子徽章 · 星瑶";
            lv_qrcode_update(qr, card, sizeof(card) - 1);
            lv_obj_set_pos(qr, 149, 159);
            lv_obj_set_style_border_width(qr, 8, 0);
            lv_obj_set_style_border_color(qr, C(0xe6f0f4), 0);
            Text("把星光带在身边", 65, 347, 336, 0x98a9b4);
        } else if (view_ == View::Walk) {
            Tabs("今日活动", "一起出发");
            info_[0] = Text("", 65, 163);
            Number(info_[0]);
            info_[1] = Text("", 65, 226, 336, 0x98a9b4);
            if (tab_)
                Button(model.outing ? "结束同行" : "一起出发", 113, 279, 240, 48, 50, true);
            else {
                gauges_[0] = Gauge(283);
                Text("每日目标 2000 步", 65, 307, 336, 0x98a9b4);
            }
            Text(tab_ ? "返回徽章也会继续记录" : "每 100 步获得 5 金币", 65, 347, 336, 0x98a9b4);
        } else if (view_ == View::Focus) {
            Tabs("专注", "提醒");
            if (!tab_) {
                info_[0] = Text("", 65, 159);
                Number(info_[0]);
                info_[2] = Text("", 65, 215, 336, 0x98a9b4);
                Button("15分", 92, 259, 88, 40, 61, model.focus_length == 15);
                Button("25分", 189, 259, 88, 40, 62, model.focus_length == 25);
                Button("45分", 286, 259, 88, 40, 63, model.focus_length == 45);
                info_[1] = Button("开始", 92, 320, 180, 48, 60, true);
                Button("重置", 282, 320, 92, 48, 64);
            } else {
                Button(model.state.water ? "喝水  60分钟   已开" : "喝水  60分钟   已关", 76, 170,
                       314, 56, 65, model.state.water);
                Button(model.state.rest ? "休息  30分钟   已开" : "休息  30分钟   已关", 76, 244,
                       314, 56, 66, model.state.rest);
                Text("轻提示音 · 徽章文字提醒", 65, 334, 336, 0x98a9b4);
            }
        } else if (view_ == View::Rhythm) {
            Text("听见身边的节奏", 65, 116, 336, 0x98a9b4);
            history_.fill(0);
            for (int i = 0; i < 16; ++i) {
                auto bar = lv_obj_create(root_);
                bars_[i] = bar;
                lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_size(bar, 11, 4);
                lv_obj_set_pos(bar, 86 + i * 19, 279);
                lv_obj_set_style_border_width(bar, 0, 0);
                lv_obj_set_style_radius(bar, 4, 0);
                lv_obj_set_style_bg_color(bar, C(i % 3 ? 0x6ee5e7 : 0xb1a1e8), 0);
            }
            Text("声音能量", 65, 310, 336, 0x6ee5e7);
            Text("退出即停止采音", 65, 347, 336, 0x98a9b4);
        }
        Update();
    }
    void Handle(int id) {
        Emit(Item::Activity);
        if (!id) {
            Back();
            return;
        }
        if (id >= 1 && id <= 8) {
            switch (id) {
                case 1:
                    Open(View::Feed);
                    break;
                case 2:
                    Open(View::Stats);
                    break;
                case 3:
                    Emit(Item::Chat);
                    break;
                case 4:
                    Open(View::Card);
                    break;
                case 5:
                    Open(View::Walk);
                    break;
                case 6:
                    Open(View::Focus);
                    break;
                case 7:
                    Open(View::Rhythm);
                    break;
                case 8:
                    Emit(Item::Settings);
                    break;
            }
        } else if (id == 20) {
            launcher_page_ = 1 - launcher_page_;
            Build();
        } else if (id == 30 || id == 31) {
            tab_ = id - 30;
            Build();
        } else if (id >= 40 && id <= 42) {
            int result = model.Feed(id - 40);
            Update();
            Set(info_[1], result == 1    ? model.reaction
                          : result == -1 ? "已经吃饱啦，等会再来"
                                         : "金币不够，摸摸或一起活动吧");
            if (result == 1) {
                Emit(Item::Save);
                Emit(Item::FeedHappy);
            }
        } else if (id == 50) {
            model.outing = !model.outing;
            if (model.outing)
                model.outing_seconds = model.outing_steps = 0;
            Build();
        } else if (id == 60) {
            model.FocusToggle();
            Update();
        } else if (id >= 61 && id <= 63) {
            model.FocusSelect(id == 61 ? 15 : id == 62 ? 25 : 45);
            Build();
        } else if (id == 64) {
            model.focus_running = false;
            model.focus_left = 0;
            Update();
        } else if (id == 65 || id == 66) {
            if (id == 65) {
                model.state.water = !model.state.water;
                model.water_elapsed = 0;
            } else {
                model.state.rest = !model.state.rest;
                model.rest_elapsed = 0;
            }
            Emit(Item::Save);
            Build();
        }
    }
};
