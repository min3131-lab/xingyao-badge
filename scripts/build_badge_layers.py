#!/usr/bin/env python3
"""把已对齐的头部和身体素材转换为 LVGL 独立动画图层。"""
import argparse
from pathlib import Path

from PIL import Image

from convert_badge_portrait import encode_rgb565, prepare_image


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BOARD_DIRECTORY = PROJECT_ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75'
HEAD_RECT = (0, 0, 466, 350)
BODY_RECT = (0, 240, 466, 226)


def prepare_head(source):
    """沿用主图的中心裁切坐标，同时保留头部边缘的透明度。"""
    with Image.open(source) as source_image:
        if 'A' not in source_image.getbands():
            raise ValueError('头部源素材必须包含透明通道。')
        image = source_image.convert('RGBA')
    side = min(image.size) / 1.2
    left = (image.width - side) / 2
    top = (image.height - side) / 2
    image = image.crop((round(left), round(top), round(left + side), round(top + side)))
    return image.resize((466, 466), Image.Resampling.LANCZOS)


def crop_rect(image, rect):
    x, y, width, height = rect
    return image.crop((x, y, x + width, y + height))


def build_layers(asset_dir, output_dir):
    head = crop_rect(prepare_head(asset_dir / '星璃-头部层.png'), HEAD_RECT)
    body = crop_rect(prepare_image(asset_dir / '星璃-身体层-v2.png'), BODY_RECT)
    alpha = head.getchannel('A').tobytes()
    if not any(value == 0 for value in alpha) or not any(value > 0 for value in alpha):
        raise ValueError('头部图层必须同时包含完全透明区域和可见区域。')
    # LVGL 的 RGB565A8 是完整 RGB565 平面后接 A8 平面，不是逐像素交错。
    head_pixels = encode_rgb565(head.convert('RGB')) + alpha
    body_pixels = encode_rgb565(body)
    assert len(head_pixels) == HEAD_RECT[2] * HEAD_RECT[3] * 3
    assert len(body_pixels) == BODY_RECT[2] * BODY_RECT[3] * 2
    layers = (('head', HEAD_RECT, 'RGB565A8', head_pixels),
              ('body', BODY_RECT, 'RGB565', body_pixels))

    header = '''// 自动生成，请通过 scripts/build_badge_layers.py 更新。
#pragma once
#include "lvgl.h"
#include "sdkconfig.h"

'''
    for name, rect, _, _ in layers:
        for suffix, value in zip(('X', 'Y', 'WIDTH', 'HEIGHT'), rect):
            header += f'#define ANIME_BADGE_{name.upper()}_{suffix} {value}\n'
    header += '''
#if CONFIG_ANIME_BADGE_DESKTOP
#ifdef __cplusplus
extern "C" {
#endif
LV_IMAGE_DECLARE(anime_badge_head);
LV_IMAGE_DECLARE(anime_badge_body);
#ifdef __cplusplus
}
#endif
#endif
'''

    source = '''// 自动生成，请通过 scripts/build_badge_layers.py 更新。
// 头部和身体分层，允许运行时独立位移；素材已裁切，无需 PNG 解码。
#include "anime_badge_layers.h"
#if CONFIG_ANIME_BADGE_DESKTOP
'''
    for name, (_, _, width, height), color_format, pixels in layers:
        symbol = f'anime_badge_{name}'
        rows = [', '.join(f'0x{byte:02x}' for byte in pixels[i:i + 24])
                for i in range(0, len(pixels), 24)]
        source += f'static const uint8_t {symbol}_pixels[] = {{\n'
        source += ',\n'.join(rows)
        source += f'''\n}};
const lv_image_dsc_t {symbol} = {{
    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_{color_format},
                .flags = 0, .w = {width}, .h = {height}, .stride = {width * 2} }},
    .data_size = sizeof({symbol}_pixels), .data = {symbol}_pixels,
}};
'''
        print(f'{symbol}: {width}×{height}, {color_format}, {len(pixels):,} 字节')
    source += '#endif\n'
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / 'anime_badge_layers.h').write_text(header, encoding='utf-8')
    (output_dir / 'anime_badge_layers.c').write_text(source, encoding='utf-8')
    print(f'头部 alpha：范围 {min(alpha)}–{max(alpha)}，完全透明 {alpha.count(0):,} 像素，'
          f'完全不透明 {alpha.count(255):,} 像素，半透明 {sum(0 < value < 255 for value in alpha):,} 像素')
    print(f'图层像素合计：{len(head_pixels) + len(body_pixels):,} 字节')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset-dir', type=Path, default=PROJECT_ROOT / 'badge-assets')
    parser.add_argument('--output-dir', type=Path, default=BOARD_DIRECTORY)
    args = parser.parse_args()
    build_layers(args.asset_dir, args.output_dir)
