#!/usr/bin/env python3
"""把角色素材预转换为 LVGL RGB565，避免设备端缩放和 PNG 解码。"""
import argparse
from pathlib import Path
from PIL import Image


def prepare_image(source):
    """共用主图和差分表情的坐标变换，保证两者逐像素对齐。"""
    with Image.open(source) as source_image:
        image = source_image.convert('RGB')
    # 预先按圆屏裁切并适度放大角色，运行时无需缩放；保留原图供换装复用。
    side = min(image.size) / 1.2
    left = (image.width - side) / 2
    top = (image.height - side) / 2
    image = image.crop((round(left), round(top), round(left + side), round(top + side)))
    return image.resize((466, 466), Image.Resampling.LANCZOS)


def encode_rgb565(image):
    """写入 ESP32/LVGL 使用的小端 RGB565 像素。"""
    pixels = bytearray()
    for red, green, blue in image.getdata():
        value = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        pixels.extend(value.to_bytes(2, 'little'))
    return pixels


def convert(source, output, symbol):
    pixels = encode_rgb565(prepare_image(source))
    rows = [', '.join(f'0x{byte:02x}' for byte in pixels[i:i + 24]) for i in range(0, len(pixels), 24)]
    output.write_text('''// 自动生成的角色资源，请通过 scripts/convert_badge_portrait.py 更新。\n#include "lvgl.h"\n#include "sdkconfig.h"\n#if CONFIG_ANIME_BADGE_DESKTOP\nstatic const uint8_t pixels[] = {\n''' + ',\n'.join(rows) + '''\n};\nconst lv_image_dsc_t ''' + symbol + ''' = {\n    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,\n                .flags = 0, .w = 466, .h = 466, .stride = 932 },\n    .data_size = sizeof(pixels), .data = pixels,\n};\n#endif\n''')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--symbol', default='anime_badge_portrait')
    args = parser.parse_args()
    convert(args.source, args.output, args.symbol)
