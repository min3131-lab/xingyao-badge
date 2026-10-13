# 星瑶拓展页主机预览

这里直接编译生产用 `anime_badge_desktop.h` 和项目自带的 LVGL，并载入相同的 common 中文字库，输出 466×466 圆形预览。只检查布局、字形、原生图标、页面导航及二维码反复创建释放，不代表 ESP32 的帧率或触摸手感。

在项目根目录、已激活 ESP-IDF 工具链的终端运行：

```sh
cmake -S validation/desktop-render -B ~/.cache/anime-badge-desktop-preview -G Ninja
cmake --build ~/.cache/anime-badge-desktop-preview -j 8
~/.cache/anime-badge-desktop-preview/preview managed_components/78__xiaozhi-fonts/cbin/font_noto_sans_common_20_4.bin validation/desktop-preview.ppm
```

使用 Pillow 打开 PPM 并另存为 PNG 即可查看 `validation/desktop-preview.png`。

字库加载部分将 ESP32 文件中的 32 位偏移转换成主机指针；仅处理本页中文所需数据，不启用拉丁字偶距。此逻辑只用于预览，不编入设备固件。
