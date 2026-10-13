#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// 纯数据养成模型：界面、传感器和存储均不影响奖励规则，可在主机验证。
class BadgeCompanion {
public:
    struct State {
        uint32_t version = 1;
        uint32_t coins = 0, affinity = 0, seconds = 0, steps = 0, focus_minutes = 0;
        uint32_t day_seconds = 0, day_steps = 0, active_seconds = 0;
        uint32_t touch_awards = 0, walk_awards = 0, company_awards = 0;
        uint32_t last_touch_award = 0, day_id = 0;
        uint32_t fullness = 60, energy = 80, water = 0, rest = 0;
    } state;
    enum Event : uint32_t { None = 0, FocusDone = 1, Water = 2, Rest = 4, Coins = 8 };
    bool focus_running = false, outing = false;
    uint32_t focus_left = 0, focus_length = 25, outing_seconds = 0, outing_steps = 0;
    uint32_t last_steps = 0, last_active = 0, water_elapsed = 0, rest_elapsed = 0;
    uint32_t mood_until = 0;
    const char* reaction = "今天也一起出发吧";
    const char* current_mood = "安心";

    bool Restore(const State& s) {
        if (s.version != 1 || s.coins > 999999 || s.affinity > 999999 || s.fullness > 100 ||
            s.energy > 100 || s.water > 1 || s.rest > 1)
            return false;
        state = s;
        return true;
    }
    uint32_t Tick(uint32_t elapsed, uint32_t sensor_steps, uint32_t day = 0) {
        uint32_t events = None;
        // 联网后按本地日期计日；离线以累计开机 24 小时计日，重启不重置奖励额度。
        if ((day && state.day_id && day > state.day_id) || (!day && state.day_seconds >= 86400)) {
            state.day_seconds = state.day_steps = state.active_seconds = 0;
            state.touch_awards = state.walk_awards = state.company_awards = 0;
        }
        if (day)
            state.day_id = std::max<uint32_t>(day, state.day_id);
        auto old_seconds = state.seconds;
        state.seconds += elapsed;
        state.day_seconds += elapsed;
        if (state.seconds / 3600 != old_seconds / 3600) {
            state.fullness = state.fullness > 3 ? state.fullness - 3 : 0;
            state.energy = std::min<uint32_t>(100u, state.energy + 2);
        }
        uint32_t delta = sensor_steps >= last_steps ? sensor_steps - last_steps : 0;
        last_steps = sensor_steps;
        // 异常计数跳变不作为行走奖励，正常步频每秒远低于这个边界。
        if (delta > std::max<uint32_t>(20u, elapsed * 5))
            delta = 0;
        if (delta)
            last_active = state.seconds;
        uint32_t active_cost = (state.steps % 100 + delta) / 100;
        state.energy = state.energy > active_cost ? state.energy - active_cost : 0;
        state.steps += delta;
        state.day_steps += delta;
        if (last_active && state.seconds - last_active < 10)
            state.active_seconds += elapsed;
        if (outing) {
            outing_seconds += elapsed;
            outing_steps += delta;
        }
        uint32_t walk_target = std::min<uint32_t>(20u, state.day_steps / 100);
        if (walk_target > state.walk_awards) {
            Award((walk_target - state.walk_awards) * 5);
            state.walk_awards = walk_target;
            events |= Coins;
        }
        uint32_t company_target = std::min<uint32_t>(15u, state.day_seconds / 600);
        if (company_target > state.company_awards) {
            Award((company_target - state.company_awards) * 2);
            state.company_awards = company_target;
            events |= Coins;
        }
        if (focus_running) {
            if (elapsed >= focus_left) {
                focus_left = 0;
                focus_running = false;
                state.focus_minutes += focus_length;
                Award(focus_length / 5 * 2);
                state.affinity = std::min<uint32_t>(999999u, state.affinity + 4);
                state.energy = state.energy > 8 ? state.energy - 8 : 0;
                current_mood = "开心";
                reaction = "一起专注完成啦";
                mood_until = state.seconds + 90;
                events |= FocusDone;
            } else
                focus_left -= elapsed;
        }
        if (state.water && (water_elapsed += elapsed) >= 3600) {
            water_elapsed %= 3600;
            events |= Water;
        }
        if (state.rest && (rest_elapsed += elapsed) >= 1800) {
            rest_elapsed %= 1800;
            events |= Rest;
        }
        return events;
    }
    bool Touch(bool shy) {
        current_mood = shy ? "害羞" : "开心";
        reaction = shy ? "有点害羞，再陪我一会" : "被你摸摸，好开心";
        mood_until = state.seconds + 90;
        if (state.touch_awards >= 12 ||
            (state.last_touch_award && state.seconds + 1 - state.last_touch_award < 300))
            return false;
        state.last_touch_award = state.seconds + 1;
        state.touch_awards++;
        state.affinity = std::min<uint32_t>(999999u, state.affinity + 1);
        Award(5);
        return true;
    }
    static uint32_t Price(int food) { return food == 0 ? 5 : food == 1 ? 8 : 6; }
    // 返回 1=成功，0=金币不足，-1=已吃饱；消费与属性变化是同一次事务。
    int Feed(int food) {
        if (food < 0 || food > 2)
            return 0;
        if (state.fullness >= 95)
            return -1;
        auto cost = Price(food);
        if (state.coins < cost)
            return 0;
        state.coins -= cost;
        state.fullness = std::min<uint32_t>(100u, state.fullness + (food == 1 ? 24u : 16u));
        state.energy = std::min<uint32_t>(100u, state.energy + (food == 2 ? 12u : 5u));
        state.affinity = std::min<uint32_t>(999999u, state.affinity + 2);
        current_mood = "满足";
        reaction = food == 0 ? "酸甜的水果，喜欢" : food == 1 ? "饼干也分你一半" : "暖暖的，很安心";
        mood_until = state.seconds + 120;
        return 1;
    }
    void FocusToggle() {
        if (!focus_left)
            focus_left = focus_length * 60;
        focus_running = !focus_running;
    }
    void FocusSelect(uint32_t minutes) {
        if (!focus_left && (minutes == 15 || minutes == 25 || minutes == 45))
            focus_length = minutes;
    }
    const char* Mood() const {
        if (state.seconds < mood_until)
            return current_mood;
        if (state.fullness < 25)
            return "有点饿";
        if (state.energy < 30)
            return "想休息";
        return "安心";
    }
    uint32_t Level() const { return 1 + state.affinity / 20; }

private:
    void Award(uint32_t amount) { state.coins = std::min<uint32_t>(999999u, state.coins + amount); }
};

// 幅值去重力 + 回滞 + 连续步频确认：倾斜跟随不算步数，孤立摇晃不发金币。
class BadgeStepDetector {
public:
    uint32_t Sample(float magnitude, uint32_t ms) {
        if (!ready_) {
            baseline_ = magnitude;
            ready_ = true;
            return 0;
        }
        baseline_ += (magnitude - baseline_) * 0.035f;
        float motion = magnitude - baseline_;
        if (motion < -0.035f)
            armed_ = true;
        if (!armed_ || motion < 0.13f || magnitude > 2.6f)
            return 0;
        armed_ = false;
        uint32_t interval = ms - last_peak_;
        if (last_peak_ && interval < 300)
            return 0;
        bool cadence = last_peak_ && interval <= 1500;
        last_peak_ = ms;
        consecutive_ = cadence ? consecutive_ + 1 : 1;
        if (consecutive_ == 3)
            return 3;
        return consecutive_ > 3 ? 1 : 0;
    }

private:
    float baseline_ = 1;
    bool ready_ = false, armed_ = false;
    uint32_t last_peak_ = 0, consecutive_ = 0;
};
