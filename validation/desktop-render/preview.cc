// 主机使用生产桌面组件和相同素材进行真实 LVGL 渲染；不模拟硬件帧率。
#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
#include "anime_badge_desktop.h"

static std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    assert(file.good());
    return {std::istreambuf_iterator<char>(file), {}};
}
static uint32_t U32(const uint8_t* p) {
    uint32_t value;
    std::memcpy(&value, p, 4);
    return value;
}
static uint16_t U16(const uint8_t* p) {
    uint16_t value;
    std::memcpy(&value, p, 2);
    return value;
}
static void Snapshot(lv_obj_t* root, const char* path) {
    lv_obj_update_layout(root);
    auto snapshot = lv_snapshot_take(root, LV_COLOR_FORMAT_RGB565);
    assert(snapshot);
    auto file = std::fopen(path, "wb");
    assert(file);
    std::fprintf(file, "P6\n466 466\n255\n");
    for (int y = 0; y < 466; ++y) {
        for (int x = 0; x < 466; ++x) {
            auto pixel = U16(static_cast<const uint8_t*>(lv_draw_buf_goto_xy(snapshot, x, y)));
            // 仅在主机输出中模拟圆形面板遮挡，不修改设备素材。
            bool inside = (x - 232.5) * (x - 232.5) + (y - 232.5) * (y - 232.5) <= 233 * 233;
            unsigned char rgb[] = {static_cast<unsigned char>(((pixel >> 11) * 255) / 31),
                                   static_cast<unsigned char>((((pixel >> 5) & 63) * 255) / 63),
                                   static_cast<unsigned char>(((pixel & 31) * 255) / 31)};
            if (!inside)
                rgb[0] = rgb[1] = rgb[2] = 0;
            std::fwrite(rgb, 1, 3, file);
        }
    }
    std::fclose(file);
    lv_draw_buf_destroy(snapshot);
}
int main(int argc, char** argv) {
    assert(argc == 3);
    auto font_data = Read(argv[1]);
    lv_init();
    auto display = lv_display_create(466, 466);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    static uint8_t buffer[466 * 20 * 2];
    lv_display_set_buffers(display, buffer, nullptr, sizeof(buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(
        display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    // 将设备字库中的 32 位指针偏移转换为主机结构，复用完全相同的中文字形和行高。
    auto base = font_data.data() + U32(font_data.data() + 24);
    auto bits = U16(base + 18);
    std::vector<lv_font_fmt_txt_cmap_t> cmaps(bits & 511);
    auto maps = base + U32(base + 8);
    for (size_t i = 0; i < cmaps.size(); ++i) {
        auto entry = maps + i * 20;
        auto& map = cmaps[i];
        map.range_start = U32(entry);
        map.range_length = U16(entry + 4);
        map.glyph_id_start = U16(entry + 6);
        map.unicode_list =
            U32(entry + 8) ? reinterpret_cast<const uint16_t*>(maps + U32(entry + 8)) : nullptr;
        map.glyph_id_ofs_list = U32(entry + 12) ? maps + U32(entry + 12) : nullptr;
        map.list_length = U16(entry + 16);
        map.type = static_cast<lv_font_fmt_txt_cmap_type_t>(entry[18]);
    }
    lv_font_fmt_txt_dsc_t descriptor{};
    descriptor.glyph_bitmap = base + U32(base);
    descriptor.glyph_dsc =
        reinterpret_cast<const lv_font_fmt_txt_glyph_dsc_t*>(base + U32(base + 4));
    descriptor.cmaps = cmaps.data();
    descriptor.cmap_num = cmaps.size();
    descriptor.bpp = (bits >> 9) & 15;
    descriptor.bitmap_format = bits >> 14;
    descriptor.stride = base[20];
    lv_font_t font{};
    font.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    font.line_height = U32(font_data.data() + 12);
    font.base_line = U32(font_data.data() + 16);
    // 此页全为中文，不使用拉丁字偶距。
    font.kerning = LV_FONT_KERNING_NONE;
    font.dsc = &descriptor;
    font.fallback = &lv_font_montserrat_14;
    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, &font, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_mem_monitor_t before{}, after{};
    lv_mem_monitor(&before);
    AnimeBadgeDesktop desktop;
    std::vector<AnimeBadgeDesktop::Item> selected;
    auto root = desktop.Create(screen, nullptr, 0, [&](auto item) { selected.push_back(item); });
    Snapshot(screen, argv[2]);
    lv_mem_monitor(&after);
    std::printf("桌面对象主机堆增量：%zu 字节（不代表 ESP32 实测）\n",
                before.free_size - after.free_size);
    // 渲染生产页面，按按钮验证导航及食物交易，不通过浏览器重画固件界面。
    using View = AnimeBadgeDesktop::View;
    const View views[] = {View::Feed, View::Stats, View::Card,
                          View::Walk, View::Focus, View::Rhythm};
    for (int i = 0; i < 6; ++i) {
        desktop.Open(views[i]);
        desktop.model.Touch(false);
        desktop.Update();
        char path[1024];
        std::snprintf(path, sizeof(path), "%s-%d.ppm", argv[2], i);
        Snapshot(screen, path);
        if (views[i] == View::Stats || views[i] == View::Walk || views[i] == View::Focus) {
            for (uint32_t j = 0; j < lv_obj_get_child_count(root); ++j) {
                auto child = lv_obj_get_child(root, j);
                if (reinterpret_cast<intptr_t>(lv_obj_get_user_data(child)) == 31) {
                    lv_obj_send_event(child, LV_EVENT_SHORT_CLICKED, nullptr);
                    break;
                }
            }
            std::snprintf(path, sizeof(path), "%s-%d-tab.ppm", argv[2], i);
            Snapshot(screen, path);
        }
        assert(desktop.Back());
        assert(desktop.view() == View::Launcher);
    }
    lv_mem_monitor_t steady{}, repeated{};
    lv_mem_monitor(&steady);
    for (int i = 0; i < 100; ++i) {
        desktop.Open(View::Card);
        desktop.Back();
    }
    lv_mem_monitor(&repeated);
    assert(steady.free_size == repeated.free_size);
    std::puts("页面导航、二维码反复创建释放检查通过");
    lv_obj_delete(root);
}
