# Waveshare ESP32-S3-Touch-AMOLED-1.75


[ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm) ESP32-S3-Touch-AMOLED-1.75 is a high performance, highly integrated microcontroller development board designed by Waveshare.\
In the smaller form, the 1.75-inch capacitive HD AMOLED screen, highly integrated power management chip, six-axis sensor (three-axis accelerometer and three-axis gyroscope), RTC, low-power audio codec chip and echo cancellation circuit are mounted on the board, which is convenient for development and embedding into the product.


## images

| [ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm)                                                                                                                                                                                           |
|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/800x800/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-1.75-1.jpg"> |
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-1.75-b-3.jpg"> |
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-1.75-b-1.jpg"> |



## 星瑶徽章变体（466×466）

本目录的 `esp32-s3-touch-amoled-1.75c-anime-badge` 变体使用 ESP-IDF 5.5.5。
BOOT 短按切换徽章与拓展桌面；徽章上滑/长按展开，桌面下滑收起，应用右滑或点击返回。
另一颗 PWR 电源键短按渐变息屏/唤醒（400ms，可中途反向），长按约 4 秒保留硬件关机；黑屏时 BOOT 先唤醒再切换。短按读取 AXP2101 的 `INTSTS2`（0x49）bit3，沿用 [XPowersLib](https://github.com/lewisxhe/XPowersLib/blob/master/src/XPowersAXP2101.hpp) 与本板官方例程的事件定义，100ms 主循环轮询，不增加任务栈。息屏停止绘制、触摸、六轴跟随/计步、对话和声律采音，专注、提醒、陪伴计时继续。每 10 秒输出内部堆及 PSRAM 剩余量和最大连续块，读数不启动音频。
两页入口为投喂、属性、对话、名片 / 同行、专注、声律、设置。主界面保留连续头身运动、自然眨眼、触摸开心/害羞；六轴跟随不会触发表情。

养成初始金币为零：有效摸摸 +5，冷却 5 分钟，每日最多 12 次；每 100 步 +5，每日最多 100；开机陪伴每 10 分钟 +2，每日最多 30；完成 15/25/45 分钟专注分别 +6/+10/+18。鲜果/饼干/热饮分别消耗 5/8/6 金币，已吃饱不扣币。金币、亲密度、精力、饱腹、累计活动与提醒开关存于独立 NVS `xingyao/companion_v1`；交易后立即排队保存，其他累计数据每分钟保存。突然断电可能损失最后一分钟活动记录。

活动计数复用 QMI8658 的既有 50Hz 采样，去重力幅值、回滞和三步连续步频确认可过滤静置、普通倾斜和孤立摇晃。它是佩戴活动估计，仍需要按实际佩戴位置校准；连续刻意摇晃可能被计入，不用于运动医疗测量。同行记录开机期间的时间与步数，没有 GPS 或蓝牙寻人。日统计联网后采用 UTC+8 日期，离线按累计开机 24 小时计日；陪伴天数按累计开机时间计算。

专注与同行退出页面后继续计时，重启结束未完成的会话，不发放未完成奖励。喝水/休息提醒分别为 60/30 分钟；扬声器播放提示音并显示短提示，音量为零时保持静音。名片为本地生成的文字二维码，内容为星瑶的同行者及签名，没有虚构社交账号。

声律只在页面打开时复用 AudioService 输入任务读取真实麦克风能量，固定缓冲区、20Hz 局部柱形更新，无 FFT、语音识别或网络上传；退出请求采音任务关闭输入。首次使用音频功能会初始化共用声卡/编解码资源，语音前端仍按需启动。普通徽章冷启动不启动音频和网络。

原桌面图片保留在源码资源目录用于回退，但不再打包；新桌面使用 LVGL 原生组件。二维码复用 [LVGL / Nayuki](https://github.com/lvgl/lvgl/tree/master/src/libs/qrcode)，与原 Emote 依赖共用一份生成器实现；传感器复用 [SensorLib](https://github.com/lewisxhe/SensorLib/blob/master/src/SensorQMI8658.hpp) 的现有接入。

构建：激活 IDF 5.5.5 后运行 `./scripts/build_badge.sh`。主机回归运行 `python3 -m unittest discover -s scripts/tests -v`；真实 LVGL 页面渲染见 `validation/desktop-render`。主机渲染和编译不能代替触摸、录音、扬声器、联网对话及佩戴计步的实机验收。
