#pragma once

#include <esp_random.h>
#include <nvs.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include "anime_badge_expressions.h"
#include "anime_badge_face_mask.h"
#include "anime_badge_head_cache.h"
#include "anime_badge_layers.h"
#include "assets.h"
#include "lvgl_theme.h"

#if CONFIG_ANIME_BADGE_DESKTOP

#include "anime_badge_desktop.h"

// 角色动画独立于联网与语音状态；小智仅作为桌面的附加入口。
class AnimeBadgeDisplay : public CustomLcdDisplay {
public:
    using CustomLcdDisplay::CustomLcdDisplay;
    std::function<void()> on_activity;
    std::function<void()> on_wifi_config;
    void SetSteps(uint32_t steps) { steps_.store(steps); }
    void ToggleDesktop() {
        DisplayLockGuard lock(this);
        HandleAction(Action::Desktop);
    }

    ~AnimeBadgeDisplay() override {
        DisplayLockGuard lock(this);
        if (companion_timer_)
            lv_timer_delete(companion_timer_);
        if (meter_timer_)
            lv_timer_delete(meter_timer_);
        if (motion_timer_)
            lv_timer_delete(motion_timer_);
        if (head_)
            lv_image_set_src(head_, &anime_badge_head);
        if (head_lower_)
            lv_obj_delete(head_lower_);
        if (face_)
            lv_image_set_src(face_, &anime_badge_blink_patch);
        head_cache_.Reset();
        face_masks_.Reset();
    }

    void SetupUI() override {
        if (IsSetupUICalled())
            return;
        current_theme_ = LvglThemeManager::GetInstance().GetTheme("dark");
        CustomLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        // 角色使用预转换 RGB565 数据，无需保留上游的 2MB PNG 解码缓存。
        lv_image_cache_resize(0, true);
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        auto screen = lv_screen_active();
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
        // 身体和头部各自连续运动；表情贴片与头部共用同一个颈部旋转轴。
        body_ = lv_image_create(container_);
        lv_image_set_src(body_, &anime_badge_body);
        lv_obj_set_pos(body_, ANIME_BADGE_BODY_X, ANIME_BADGE_BODY_Y);
        lv_image_set_pivot(body_, 233, ANIME_BADGE_BODY_HEIGHT);
        head_ = lv_image_create(container_);
        lv_image_set_src(head_, &anime_badge_head);
        lv_image_set_pivot(head_, 233, 320);
        // 缓存上部直接复制 RGB565，下部透明层只负责肩颈和发梢，二者严格同移。
        head_lower_ = lv_image_create(container_);
        lv_obj_add_flag(head_lower_, LV_OBJ_FLAG_HIDDEN);
        face_ = lv_image_create(container_);
        lv_image_set_src(face_, &anime_badge_blink_patch);
        lv_obj_set_pos(face_, ANIME_BADGE_FACE_X, ANIME_BADGE_FACE_Y);
        lv_image_set_pivot(face_, 233 - ANIME_BADGE_FACE_X, 320 - ANIME_BADGE_FACE_Y);
        lv_obj_set_style_opa(face_, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(container_, LV_OBJ_FLAG_CLICKABLE);
        Bind(container_, LV_EVENT_SHORT_CLICKED, Action::Interact);
        // 长按与上滑都可打开桌面，佩戴时不用常驻按钮遮挡角色。
        lv_obj_add_event_cb(
            container_,
            [](lv_event_t* event) {
                auto self = static_cast<AnimeBadgeDisplay*>(lv_event_get_user_data(event));
                self->HandleAction(Action::Desktop);
                if (auto input = lv_indev_active())
                    lv_indev_wait_release(input);
            },
            LV_EVENT_LONG_PRESSED, this);

        LoadCompanion();
        desktop_ = desktop_ui_.Create(screen, nullptr, 0, [this](AnimeBadgeDesktop::Item item) {
            auto& app = Application::GetInstance();
            switch (item) {
                case AnimeBadgeDesktop::Item::Chat:
                    HandleAction(Action::Chat);
                    break;
                case AnimeBadgeDesktop::Item::Settings:
                    HandleAction(Action::Settings);
                    break;
                case AnimeBadgeDesktop::Item::FeedHappy:
                    ShowPage(Page::Portrait);
                    React(false);
                    Toast(desktop_ui_.model.reaction);
                    break;
                case AnimeBadgeDesktop::Item::Save:
                    SaveCompanion();
                    break;
                case AnimeBadgeDesktop::Item::RhythmStart:
                    app.CancelPendingChat();
                    app.SetLocalAudioMonitor(true);
                    if (meter_timer_)
                        lv_timer_resume(meter_timer_);
                    break;
                case AnimeBadgeDesktop::Item::RhythmStop:
                    app.SetLocalAudioMonitor(false);
                    if (meter_timer_)
                        lv_timer_pause(meter_timer_);
                    break;
                case AnimeBadgeDesktop::Item::Badge:
                    LeaveChat();
                    ShowPage(Page::Portrait);
                    break;
                case AnimeBadgeDesktop::Item::Activity:
                    if (on_activity)
                        app.Schedule([cb = on_activity]() { cb(); });
                    break;
            }
        });

        settings_ = Panel(screen, 306, 264);
        auto heading = Label(settings_, "徽章设置");
        lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 5);
        auto brightness = Label(settings_, "亮度");
        lv_obj_set_pos(brightness, 25, 48);
        brightness_ = MakeSlider(settings_, 88, 10, 100, Action::Brightness);
        auto volume = Label(settings_, "音量");
        lv_obj_set_pos(volume, 25, 116);
        volume_ = MakeSlider(settings_, 156, 0, 100, Action::Volume);
        MakeButton(settings_, "重新配网", 60, 197, 186, 43, Action::Wifi);

        // 沿用上游通知、电量、网络状态和中文激活提示，避免遮挡圆屏边缘。
        lv_obj_set_width(top_bar_, 270);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 44);
        lv_obj_set_size(status_bar_, 230, 34);
        lv_obj_set_style_pad_all(status_bar_, 0, 0);
        lv_obj_set_width(status_label_, 230);
        lv_obj_set_width(notification_label_, 230);
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 106);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_size(bottom_bar_, 324, 140);
        lv_obj_align(bottom_bar_, LV_ALIGN_TOP_MID, 0, 211);
        lv_obj_set_size(chat_message_label_, 308, 58);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(bottom_bar_, LV_DIR_VER);
        // 滚动长字幕时不把上滑解释为打开桌面；返回仍使用底部按钮。
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
        lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, 3);

        chat_title_ = Label(screen, "星瑶 · 对话");
        lv_obj_align(chat_title_, LV_ALIGN_TOP_MID, 0, 66);
        chat_orb_ = Panel(screen, 64, 64);
        lv_obj_set_style_radius(chat_orb_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(chat_orb_, lv_color_hex(0x153b42), 0);
        lv_obj_align(chat_orb_, LV_ALIGN_TOP_MID, 0, 143);
        auto mic = Label(chat_orb_, LV_SYMBOL_CALL);
        lv_obj_set_style_text_font(mic, &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(mic, lv_color_hex(0x6ee5e7), 0);
        lv_obj_center(mic);
        nav_ = Panel(screen, 284, 48);
        lv_obj_set_style_bg_opa(nav_, LV_OPA_TRANSP, 0);
        lv_obj_align(nav_, LV_ALIGN_BOTTOM_MID, 0, -61);
        back_button_ = MakeButton(nav_, "返回", 0, 0, 130, 46, Action::Back);
        chat_button_ = MakeButton(nav_, "开始", 154, 0, 130, 46, Action::Chat);
        // 复用 LVGL 手势识别，消耗本次触摸，避免松手后误点露出的角色。
        lv_obj_add_event_cb(
            screen,
            [](lv_event_t* event) {
                auto input = lv_indev_active();
                if (!input)
                    return;
                auto self = static_cast<AnimeBadgeDisplay*>(lv_event_get_user_data(event));
                auto direction = lv_indev_get_gesture_dir(input);
                if (direction == LV_DIR_TOP && self->page_ == Page::Portrait) {
                    self->HandleAction(Action::Desktop);
                    lv_indev_wait_release(input);
                } else if (direction == LV_DIR_RIGHT || direction == LV_DIR_BOTTOM) {
                    self->HandleAction(Action::Back);
                    lv_indev_wait_release(input);
                }
            },
            LV_EVENT_GESTURE, this);
        hint_ = Label(screen, "摸摸赚金币 · 按键拓展");
        lv_obj_set_width(hint_, 300);
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(hint_, lv_color_hex(0x0d171d), 0);
        lv_obj_set_style_bg_opa(hint_, LV_OPA_70, 0);
        lv_obj_set_style_pad_all(hint_, 8, 0);
        lv_obj_set_style_radius(hint_, 16, 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -43);
        hint_until_ = lv_tick_get() + 6500;
        next_blink_ = lv_tick_get() + 2300;
        motion_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<AnimeBadgeDisplay*>(lv_timer_get_user_data(timer))->Animate();
            },
            40, this);
        companion_tick_ = lv_tick_get();
        companion_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<AnimeBadgeDisplay*>(lv_timer_get_user_data(timer))->TickCompanion();
            },
            1000, this);
        meter_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto self = static_cast<AnimeBadgeDisplay*>(lv_timer_get_user_data(timer));
                self->desktop_ui_.Meter(
                    Application::GetInstance().GetAudioService().GetInputLevel());
            },
            50, this);
        lv_timer_pause(meter_timer_);
        // 记录实际 LVGL 绘制耗时，不把定时器目标频率当作实测帧率。
        lv_display_add_event_cb(
            display_,
            [](lv_event_t* event) {
                auto self = static_cast<AnimeBadgeDisplay*>(lv_event_get_user_data(event));
                self->render_start_ = esp_timer_get_time();
            },
            LV_EVENT_RENDER_START, this);
        lv_display_add_event_cb(
            display_,
            [](lv_event_t* event) {
                auto self = static_cast<AnimeBadgeDisplay*>(lv_event_get_user_data(event));
                uint32_t elapsed = (esp_timer_get_time() - self->render_start_) / 1000;
                self->render_total_ += elapsed;
                self->render_count_++;
                if (elapsed > self->render_max_)
                    self->render_max_ = elapsed;
            },
            LV_EVENT_RENDER_READY, this);
        // 离线首次开机也要有中文字库，不能等联网激活后才载入。
        void* font_data = nullptr;
        size_t font_size = 0;
        if (Assets::GetInstance().GetAssetData("font_noto_sans_common_20_4.bin", font_data,
                                               font_size)) {
            auto font = std::make_shared<LvglCBinFont>(font_data);
            if (SetTextFont(font))
                ESP_LOGI("AnimeBadge", "离线中文字库已加载");
        }
        face_masks_.Initialize();
        head_cache_.Initialize();
        int initial_angle = AnimeBadgeHeadCache::PoseCorrection();
        if (auto initial = head_cache_.Get(0, initial_angle)) {
            lv_image_set_src(head_, initial);
            lv_image_set_src(head_lower_, head_cache_.GetLower(0));
            lv_obj_set_pos(head_lower_, 0, AnimeBadgeHeadCache::LowerY());
            lv_obj_remove_flag(head_lower_, LV_OBJ_FLAG_HIDDEN);
            cached_head_ = initial;
        } else {
            lv_image_set_rotation(head_, initial_angle);
        }
        lv_image_set_rotation(face_, initial_angle);
        // 首个统计窗口从缓存构建结束开始，不把启动预绘制时间算进运行刷新率。
        metrics_start_ = lv_tick_get();
        render_total_ = render_count_ = render_max_ = 0;
        ApplyPalette();
        ShowPage(Page::Portrait);
    }

    void SetTheme(Theme* theme) override {
        CustomLcdDisplay::SetTheme(theme);
        if (!IsSetupUICalled())
            return;
        DisplayLockGuard lock(this);
        ApplyPalette();
    }

    void SetStatus(const char* status) override {
        CustomLcdDisplay::SetStatus(status);
        DisplayLockGuard lock(this);
        if (!chat_button_)
            return;
        auto state = Application::GetInstance().GetDeviceState();
        (void)state;
        lv_label_set_text(lv_obj_get_child(chat_button_, 0), chat_requested_ ? "结束" : "开始");
        // 只响应用户主动打开的对话页，后台状态不抢占徽章。
        RefreshVisibility();
    }

    void SetChatMessage(const char* role, const char* content) override {
        DisplayLockGuard lock(this);
        if (!chat_message_label_)
            return;
        // 后台保留配网/激活信息，只有主动进入小智时才覆盖角色。
        std::string message = content ? content : "";
        if (message.size() > 2048) {
            size_t end = 2048;
            while (end && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80)
                --end;
            message.resize(end);
        }
        lv_label_set_text(chat_message_label_, message.c_str());
        lv_obj_scroll_to_y(bottom_bar_, 0, LV_ANIM_OFF);
        RefreshVisibility();
    }

    void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        if (!face_ || !chat_mode_ || !emotion)
            return;
        if (!strcmp(emotion, "happy") || !strcmp(emotion, "laughing"))
            React(false);
        else if (!strcmp(emotion, "shy") || !strcmp(emotion, "loving"))
            React(true);
    }

    // 六轴只提供位移和倾斜目标，不触发表情或预设弹跳；视觉更新在 LVGL 任务执行。
    void SetMotion(float x, float y) {
        motion_x_.store(std::isfinite(x) ? x : 0.0f, std::memory_order_relaxed);
        motion_y_.store(std::isfinite(y) ? y : 0.0f, std::memory_order_relaxed);
    }

    void PrepareForScreenOff() {
        DisplayLockGuard lock(this);
        // 屏幕关闭后结束可见页面的采音；专注、提醒和养成计时仍然继续。
        if (chat_mode_) {
            LeaveChat();
            ShowPage(Page::Portrait);
        }
        if (desktop_ui_.view() == AnimeBadgeDesktop::View::Rhythm)
            desktop_ui_.Launcher();
        SaveCompanion();
    }

    // 渐变期间冻结大图绘制，让亮度动画保持均匀；面板等渐暗完成才关闭。
    void SetScreenFading(bool fading, lv_indev_t* touch_input) {
        DisplayLockGuard lock(this);
        screen_fading_ = fading;
        if (touch_input)
            lv_indev_enable(touch_input, !screen_off_ && !screen_fading_);
        RefreshVisibility();
    }

    void SetScreenOff(bool off, lv_indev_t* touch_input) {
        DisplayLockGuard lock(this);
        screen_off_ = off;
        if (touch_input)
            lv_indev_enable(touch_input, !off && !screen_fading_);
        RefreshVisibility();
        // 手动息屏停止面板输出与 LVGL 区域刷新，亮屏时重绘当前页面。
        lv_display_enable_invalidation(display_, !off);
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, !off));
        if (!off)
            lv_obj_invalidate(lv_screen_active());
    }

    void SetPowerSaveMode(bool on) override {
        DisplayLockGuard lock(this);
        // 徽章待机只降亮度，离线与省电都保留动作；手动息屏才停止绘制。
        sleeping_ = on;
        RefreshVisibility();
    }

private:
    enum class Page { Portrait, Desktop, Settings };
    enum class Action { Back, Desktop, Settings, Chat, Brightness, Volume, Wifi, Interact };
    lv_obj_t *body_ = nullptr, *head_ = nullptr, *head_lower_ = nullptr, *face_ = nullptr;
    lv_obj_t *desktop_ = nullptr, *settings_ = nullptr, *nav_ = nullptr;
    lv_obj_t *chat_button_ = nullptr, *hint_ = nullptr;
    lv_obj_t *chat_title_ = nullptr, *chat_orb_ = nullptr, *back_button_ = nullptr;
    lv_obj_t *brightness_ = nullptr, *volume_ = nullptr;
    Page page_ = Page::Portrait;
    bool sleeping_ = false, screen_off_ = false, screen_fading_ = false, chat_mode_ = false;
    bool expression_active_ = false, blink_expression_ = false;
    uint32_t hint_until_ = 0, next_blink_ = 0;
    uint32_t expression_start_ = 0, expression_duration_ = 0;
    uint32_t reaction_start_ = 0, last_touch_ = 0, touch_count_ = 0;
    uint32_t metrics_start_ = 0, render_total_ = 0, render_count_ = 0, render_max_ = 0;
    int64_t render_start_ = 0;
    lv_timer_t* motion_timer_ = nullptr;
    lv_timer_t *companion_timer_ = nullptr, *meter_timer_ = nullptr;
    uint32_t companion_tick_ = 0, last_save_tick_ = 0, battery_tick_ = 0;
    bool chat_requested_ = false, hint_badge_only_ = true;
    std::atomic<uint32_t> steps_{0};
    AnimeBadgeHeadCache head_cache_;
    AnimeBadgeFaceMask face_masks_;
    AnimeBadgeDesktop desktop_ui_;
    const lv_image_dsc_t* cached_head_ = nullptr;
    std::atomic<float> motion_x_{0}, motion_y_{0};
    float follow_x_ = 0, follow_y_ = 0, velocity_x_ = 0, velocity_y_ = 0;
    float follow_angle_ = 0, velocity_angle_ = 0;
    uint32_t last_animation_ = 0;

    lv_obj_t* Label(lv_obj_t* parent, const char* text) {
        auto label = lv_label_create(parent);
        lv_label_set_text(label, text);
        return label;
    }

    lv_obj_t* Panel(lv_obj_t* parent, int width, int height) {
        auto panel = lv_obj_create(parent);
        lv_obj_set_size(panel, width, height);
        lv_obj_set_style_pad_all(panel, 0, 0);
        lv_obj_set_style_radius(panel, 24, 0);
        lv_obj_set_style_border_width(panel, 0, 0);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x0d171d), 0);
        lv_obj_set_style_text_color(panel, lv_color_hex(0xe6f0f4), 0);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(panel, LV_ALIGN_CENTER, 0, 5);
        return panel;
    }

    lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, int x, int y, int w, int h,
                         Action action) {
        auto button = lv_button_create(parent);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, w, h);
        lv_obj_set_style_radius(button, 18, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x15313b), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x295664), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_text_color(button, lv_color_hex(0xe6f0f4), 0);
        lv_obj_center(Label(button, text));
        Bind(button, LV_EVENT_CLICKED, action);
        return button;
    }

    lv_obj_t* MakeSlider(lv_obj_t* parent, int y, int low, int high, Action action) {
        auto slider = lv_slider_create(parent);
        lv_obj_set_size(slider, 246, 10);
        lv_obj_set_pos(slider, 30, y);
        lv_slider_set_range(slider, low, high);
        // 横向拖动滑杆只调整数值，不向上冒泡为返回手势。
        lv_obj_remove_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0x6ee5e7), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0x6ee5e7), LV_PART_KNOB);
        Bind(slider, LV_EVENT_RELEASED, action);
        return slider;
    }

    void Bind(lv_obj_t* obj, lv_event_code_t code, Action action) {
        lv_obj_set_user_data(obj, reinterpret_cast<void*>(static_cast<intptr_t>(action)));
        lv_obj_add_event_cb(
            obj,
            [](lv_event_t* event) {
                auto self = static_cast<AnimeBadgeDisplay*>(lv_event_get_user_data(event));
                auto target = static_cast<lv_obj_t*>(lv_event_get_target(event));
                auto action =
                    static_cast<Action>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
                self->HandleAction(action);
            },
            code, this);
    }

    void HandleAction(Action action) {
        // LVGL 回调只更新界面，音频、NVS、电源和配网操作转交应用主任务。
        auto& app = Application::GetInstance();
        if (on_activity)
            app.Schedule([cb = on_activity]() { cb(); });
        switch (action) {
            case Action::Back:
                ESP_LOGI("AnimeBadge", "触摸返回，页面=%d", static_cast<int>(page_));
                if (page_ == Page::Settings) {
                    ShowPage(Page::Desktop);
                } else if (page_ == Page::Desktop) {
                    if (!desktop_ui_.Back())
                        ShowPage(Page::Portrait);
                } else {
                    LeaveChat();
                    desktop_ui_.Launcher();
                    ShowPage(Page::Desktop);
                }
                break;
            case Action::Desktop: {
                bool to_badge = page_ != Page::Portrait || chat_mode_;
                LeaveChat();
                desktop_ui_.Launcher();
                ShowPage(to_badge ? Page::Portrait : Page::Desktop);
                break;
            }
            case Action::Settings:
                ShowPage(Page::Settings);
                app.Schedule([this]() {
                    auto& board = Board::GetInstance();
                    int brightness = board.GetBacklight() ? board.GetBacklight()->brightness() : 60;
                    int volume = Application::GetInstance().GetAudioOutputVolume();
                    DisplayLockGuard lock(this);
                    lv_slider_set_value(brightness_, brightness, LV_ANIM_OFF);
                    lv_slider_set_value(volume_, volume, LV_ANIM_OFF);
                });
                break;
            case Action::Chat: {
                if (chat_mode_ && chat_requested_) {
                    app.CancelPendingChat();
                    chat_requested_ = false;
                    lv_label_set_text(lv_obj_get_child(chat_button_, 0), "开始");
                    break;
                }
                chat_mode_ = chat_requested_ = true;
                ShowPage(Page::Portrait);
                lv_label_set_text(lv_obj_get_child(chat_button_, 0), "结束");
                lv_label_set_text(chat_message_label_, "正在准备对话，请稍候…");
                app.RequestChat();
                break;
            }
            case Action::Interact:
                if (page_ == Page::Portrait && !chat_mode_) {
                    auto now = lv_tick_get();
                    touch_count_ = now - last_touch_ < 1600 ? touch_count_ + 1 : 1;
                    last_touch_ = now;
                    React(touch_count_ > 1);
                    bool rewarded = desktop_ui_.model.Touch(touch_count_ > 1);
                    Toast(rewarded ? "摸摸 +5 金币" : desktop_ui_.model.reaction);
                    if (rewarded)
                        SaveCompanion();
                    ESP_LOGI("AnimeBadge", "触摸互动：%s", touch_count_ > 1 ? "害羞" : "开心");
                }
                break;
            case Action::Brightness: {
                int value = lv_slider_get_value(brightness_);
                app.Schedule([value]() {
                    if (auto light = Board::GetInstance().GetBacklight())
                        light->SetBrightness(value, true);
                });
                break;
            }
            case Action::Volume: {
                int value = lv_slider_get_value(volume_);
                app.Schedule([value]() { Application::GetInstance().SetAudioOutputVolume(value); });
                break;
            }
            case Action::Wifi:
                app.CancelPendingChat();
                chat_mode_ = true;
                ShowPage(Page::Portrait);
                if (on_wifi_config)
                    app.Schedule([cb = on_wifi_config]() { cb(); });
                break;
        }
    }

    void ShowPage(Page page) {
        // 重新进入徽章后独立统计实际绘制，避免把桌面停帧时间算成动画掉帧。
        if (page == Page::Portrait && page_ != Page::Portrait) {
            metrics_start_ = lv_tick_get();
            render_total_ = render_count_ = render_max_ = 0;
        }
        if (page_ == Page::Desktop && page != Page::Desktop)
            desktop_ui_.Hide();
        page_ = page;
        lv_obj_add_flag(desktop_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(settings_, LV_OBJ_FLAG_HIDDEN);
        if (page == Page::Desktop)
            lv_obj_remove_flag(desktop_, LV_OBJ_FLAG_HIDDEN);
        if (page == Page::Settings)
            lv_obj_remove_flag(settings_, LV_OBJ_FLAG_HIDDEN);
        if (page != Page::Portrait)
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        RefreshVisibility();
    }

    void LeaveChat() {
        chat_mode_ = chat_requested_ = false;
        auto& app = Application::GetInstance();
        app.CancelPendingChat();
    }

    void SetHidden(lv_obj_t* obj, bool hidden) {
        if (!obj)
            return;
        if (hidden)
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }

    void RefreshVisibility() {
        bool character = page_ == Page::Portrait && !chat_mode_;
        bool chat = page_ == Page::Portrait && chat_mode_;
        SetHidden(container_, !character);
        SetHidden(nav_, !chat && page_ != Page::Settings);
        SetHidden(chat_button_, !chat);
        if (back_button_)
            lv_obj_set_x(back_button_, chat ? 0 : 77);
        SetHidden(top_bar_, !chat);
        SetHidden(status_bar_, !chat);
        SetHidden(chat_title_, !chat);
        SetHidden(chat_orb_, !chat);
        SetHidden(bottom_bar_, !chat);
        SetHidden(hint_, (!character && hint_badge_only_) ||
                             static_cast<int32_t>(lv_tick_get() - hint_until_) >= 0);
        if (hint_)
            lv_obj_align(hint_, character ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0,
                         character ? -43 : 45);
        if (motion_timer_) {
            if (screen_off_ || screen_fading_ || !character)
                lv_timer_pause(motion_timer_);
            else
                lv_timer_resume(motion_timer_);
        }
    }

    void Toast(const char* text) {
        hint_badge_only_ = false;
        lv_label_set_text(hint_, text);
        hint_until_ = lv_tick_get() + 5000;
        RefreshVisibility();
    }

    void LoadCompanion() {
        nvs_handle_t nvs;
        if (nvs_open("xingyao", NVS_READONLY, &nvs) != ESP_OK)
            return;
        BadgeCompanion::State state;
        size_t size = sizeof(state);
        if (nvs_get_blob(nvs, "companion_v1", &state, &size) == ESP_OK && size == sizeof(state))
            desktop_ui_.model.Restore(state);
        nvs_close(nvs);
        ESP_LOGI("AnimeBadge", "养成存档：金币=%lu 亲密度=%lu 累计步数=%lu",
                 (unsigned long)desktop_ui_.model.state.coins,
                 (unsigned long)desktop_ui_.model.state.affinity,
                 (unsigned long)desktop_ui_.model.state.steps);
    }

    void SaveCompanion() {
        auto snapshot = desktop_ui_.model.state;
        last_save_tick_ = lv_tick_get();
        Application::GetInstance().Schedule([snapshot]() {
            nvs_handle_t nvs;
            esp_err_t err = nvs_open("xingyao", NVS_READWRITE, &nvs);
            if (err == ESP_OK) {
                err = nvs_set_blob(nvs, "companion_v1", &snapshot, sizeof(snapshot));
                if (err == ESP_OK)
                    err = nvs_commit(nvs);
                nvs_close(nvs);
            }
            if (err != ESP_OK)
                ESP_LOGW("AnimeBadge", "养成存档失败：%s", esp_err_to_name(err));
        });
    }

    void TickCompanion() {
        auto now = lv_tick_get();
        auto elapsed = (now - companion_tick_) / 1000;
        if (!elapsed)
            return;
        companion_tick_ += elapsed * 1000;
        time_t wall = time(nullptr);
        uint32_t day = wall > 1700000000 ? static_cast<uint32_t>((wall + 8 * 3600) / 86400) : 0;
        auto events = desktop_ui_.model.Tick(elapsed, steps_.load(), day);
        if (page_ == Page::Desktop)
            desktop_ui_.Update();
        RefreshVisibility();
        if (events & BadgeCompanion::FocusDone) {
            Toast("专注完成，金币已到账");
            SaveCompanion();
        } else if (events & BadgeCompanion::Water)
            Toast("喝口水，再一起继续吧");
        else if (events & BadgeCompanion::Rest)
            Toast("抬头远望，放松一下");
        else if (events & BadgeCompanion::Coins) {
            Toast("活跃奖励已到账");
            SaveCompanion();
        }
        if (events & (BadgeCompanion::FocusDone | BadgeCompanion::Water | BadgeCompanion::Rest))
            Application::GetInstance().PlayLocalReminder();
        if (now - last_save_tick_ >= 60000)
            SaveCompanion();
        if (now - battery_tick_ >= 15000) {
            battery_tick_ = now;
            Application::GetInstance().Schedule([this]() {
                int level = 0;
                bool charging = false, discharging = false;
                if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
                    DisplayLockGuard lock(this);
                    desktop_ui_.SetBattery(level, charging);
                }
            });
        }
    }

    void Expression(const lv_image_dsc_t* frame, uint32_t duration, bool blink = false) {
        lv_image_set_src(face_, face_masks_.Get(frame));
        expression_start_ = lv_tick_get();
        expression_duration_ = duration;
        expression_active_ = true;
        blink_expression_ = blink;
        lv_obj_set_style_opa(face_, LV_OPA_TRANSP, 0);
    }

    void React(bool shy) {
        reaction_start_ = lv_tick_get();
        Expression(shy ? &anime_badge_shy_patch : &anime_badge_happy_patch, shy ? 2400 : 1800);
        next_blink_ = reaction_start_ + 3000;
    }

    void Animate() {
        if (screen_off_ || page_ != Page::Portrait || chat_mode_)
            return;
        uint32_t now = lv_tick_get();
        float dt = last_animation_ ? (now - last_animation_) * 0.001f : 0.04f;
        last_animation_ = now;
        if (dt > 0.08f)
            dt = 0.08f;
        // 阻尼弹簧保留一点惯性，避免传感器噪声直接变成画面抖动。
        float target_x = std::clamp(motion_x_.load(std::memory_order_relaxed), -1.0f, 1.0f) * 32.0f;
        float target_y = std::clamp(motion_y_.load(std::memory_order_relaxed), -1.0f, 1.0f) * 24.0f;
        float damping = expf(-9.0f * dt);
        velocity_x_ = (velocity_x_ + (target_x - follow_x_) * 90.0f * dt) * damping;
        velocity_y_ = (velocity_y_ + (target_y - follow_y_) * 90.0f * dt) * damping;
        follow_x_ = std::clamp(follow_x_ + velocity_x_ * dt, -32.0f, 32.0f);
        follow_y_ = std::clamp(follow_y_ + velocity_y_ * dt, -24.0f, 24.0f);
        // 零目标保留原图自然歪头；正向依次经过回正约 17 度，再倾向另一边。
        // 角度使用与位移相同的阻尼弹簧，停止或换向时不会突然切换姿态模式。
        const float target_angle = target_x >= 0 ? target_x * 10.0f : target_x * 3.125f;
        velocity_angle_ = (velocity_angle_ + (target_angle - follow_angle_) * 90.0f * dt) * damping;
        follow_angle_ = std::clamp(follow_angle_ + velocity_angle_ * dt, -100.0f, 320.0f);
        float seconds = now * 0.001f;
        float breath = sinf(seconds * 2.1f);
        float sway = sinf(seconds * 1.35f);
        float bounce = 0;
        float tilt = 0;
        uint32_t reaction_time = now - reaction_start_;
        if (reaction_start_ && reaction_time < 1400) {
            float t = reaction_time * 0.001f;
            float decay = 1.0f - t / 1.4f;
            bounce = -18.0f * fabsf(sinf(t * 9.0f)) * decay;
            tilt = 38.0f * sinf(t * 11.0f) * decay;
        }
        int x = lroundf(4.0f * sway + follow_x_);
        int y = lroundf(3.0f * breath + bounce + follow_y_);
        int angle = lroundf(10.0f * sinf(seconds * 1.35f - 0.4f) + tilt + follow_angle_);
        int cached_angle = 0;
        auto cached = head_cache_.Get(angle, cached_angle);
        if (cached && cached != cached_head_) {
            lv_image_set_src(head_, cached);
            lv_image_set_src(head_lower_, head_cache_.GetLower(angle));
            lv_obj_remove_flag(head_lower_, LV_OBJ_FLAG_HIDDEN);
            cached_head_ = cached;
        }
        // 两段缓存同时平移，姿态旋转已经在缓存内完成，不逐帧变换大图。
        lv_obj_set_pos(head_, x, y);
        lv_obj_set_pos(head_lower_, x, y + AnimeBadgeHeadCache::LowerY());
        lv_image_set_rotation(head_, cached ? 0 : cached_angle);
        lv_obj_set_pos(face_, ANIME_BADGE_FACE_X + x, ANIME_BADGE_FACE_Y + y);
        lv_image_set_rotation(face_, cached_angle);
        // 大幅度采用整个角色共同跟随；头身相对位移限制在横向 8、纵向 4 像素内。
        // 一次设置最终坐标，避免先 set_pos 再 set_y 导致同一帧多次无效区域扩张。
        lv_obj_set_pos(body_, lroundf(sway + follow_x_ * 0.88f),
                       ANIME_BADGE_BODY_Y + lroundf(2.0f * breath + bounce + follow_y_ * 0.92f));
        if (expression_active_) {
            uint32_t age = now - expression_start_;
            uint32_t fade_in = blink_expression_ ? 45 : 120;
            uint32_t fade_out = blink_expression_ ? 65 : 180;
            uint32_t opacity = 255;
            if (age >= expression_duration_) {
                opacity = 0;
                expression_active_ = false;
            } else if (age < fade_in) {
                opacity = 255 * age / fade_in;
            } else if (age > expression_duration_ - fade_out) {
                opacity = 255 * (expression_duration_ - age) / fade_out;
            }
            lv_obj_set_style_opa(face_, opacity, 0);
        } else if (static_cast<int32_t>(now - next_blink_) >= 0) {
            Expression(&anime_badge_blink_patch, 230, true);
            next_blink_ = now + 2400 + esp_random() % 2800;
        }
        SetHidden(hint_, chat_mode_ || static_cast<int32_t>(now - hint_until_) >= 0);
        if (now - metrics_start_ >= 10000 && render_count_) {
            // 实际完成 render 的次数除以经过时间，不使用定时器目标频率代替实测。
            const float refresh_rate = render_count_ * 1000.0f / (now - metrics_start_);
            ESP_LOGI("AnimeBadge", "动画绘制：刷新=%.1f次/秒 次数=%lu 平均=%lums 最长=%lums",
                     refresh_rate, (unsigned long)render_count_,
                     (unsigned long)(render_total_ / render_count_), (unsigned long)render_max_);
            head_cache_.ReportDecodeMetrics();
            metrics_start_ = now;
            render_total_ = render_count_ = render_max_ = 0;
        }
    }

    void ApplyPalette() {
        lv_obj_set_style_bg_color(container_, lv_color_black(), 0);
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
        lv_obj_set_style_text_color(lv_screen_active(), lv_color_hex(0xe6f0f4), 0);
        for (auto label : {status_label_, notification_label_, chat_message_label_, network_label_,
                           battery_label_, mute_label_}) {
            if (label)
                lv_obj_set_style_text_color(label, lv_color_hex(0xe6f0f4), 0);
        }
        lv_obj_set_style_bg_color(top_bar_, lv_color_black(), 0);
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0x0d171d), 0);
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_90, 0);
        lv_obj_set_style_radius(bottom_bar_, 18, 0);
    }
};
using BadgeBoardDisplay = AnimeBadgeDisplay;
#else
using BadgeBoardDisplay = CustomLcdDisplay;
#endif
