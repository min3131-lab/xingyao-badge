# 星瑶徽章维护说明

功能、交互、金币规则、资源余量与限制以 [项目首页](README.md) 为准，板级实现说明见 [板级 README](main/boards/waveshare/esp32-s3-touch-amoled-1.75/README.md)。

## 构建与素材

徽章变体 `esp32-s3-touch-amoled-1.75c-anime-badge` 使用 ESP-IDF 5.5.5。激活环境后运行 `./scripts/build_badge.sh`，输出默认为 `~/.cache/anime-badge-build`。需要更新其他板型时，先阅读上游架构和 SDK 兼容说明，不要混用本徽章的构建结果。

仓库已包含设备使用的姿态库。替换形象时，保持头身比例、画布坐标与透明通道，安装 Pillow，并先通过 ESP-IDF 下载项目依赖，然后依次执行：

```sh
python scripts/build_badge_expressions.py
python scripts/build_badge_layers.py
python scripts/build_badge_pose_bank.py
./scripts/build_badge.sh
```

姿态生成器调用 LVGL 自带的 LZ4，需要本机 C 编译器。源图使用一致的 1.2 倍中心裁切和 466×466 屏幕坐标；头部上下图层与表情共享旋转轴。素材详细记录见 [素材说明](badge-assets/素材说明.md)。早期原图及桌面图标仍保留用于参考，当前原生拓展桌面不加载这些旧桌面图片。

## 发布固件

构建不会自动替换 `firmware-badge/`。验证新的二进制后，再复制应用、资源、bootloader、分区表和初始 OTA 数据，重新计算 `SHA256SUMS`。沿用分段烧录，禁止把个人设备的整片备份当作发布固件。

`generated_assets.bin` 位于 `0x11f0000`。首次安装或素材变化时必须同步更新应用与资源；仅在资源 SHA256 相同时才可单独烧录应用。完整流程见 [烧录说明](firmware-badge/烧录说明.md)。

## 验证

主机测试：`python3 -m unittest discover -s scripts/tests -v`。实际 LVGL 页面布局见 [主机预览](validation/desktop-render/README.md)。固件构建成功不等于硬件验收，应分别检查触摸、BOOT 切换、PWR 渐变、六轴跟随、投喂存档、声律采音退出、语音和提醒。

首次使用后的共享音频任务和编解码资源保留；返回或息屏结束采音，但不销毁全部音频资源。息屏停止六轴采样，因此不累计息屏期间步数。专注和陪伴计时继续，重启不恢复未完成的专注会话。

## 本机数据

`device-backup/`、串口日志、编译目录和本机配置被 Git 忽略。备份可能含配网、激活及其他设备数据，只保存在本地。仓库固件是构建产物，不包含 NVS 转储。
