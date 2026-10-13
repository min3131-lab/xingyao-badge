#!/usr/bin/env python3
"""预转换局部面部表情，保留主图并减少 Flash 占用和局部刷新面积。"""
import argparse
from pathlib import Path

from convert_badge_portrait import encode_rgb565, prepare_image


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BOARD_DIRECTORY = PROJECT_ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75'
EXPRESSIONS = (('blink', '眨眼'), ('happy', '开心'), ('shy', '害羞'))


def build_expressions(asset_dir, output_dir, rect):
    x, y, width, height = rect
    if min(x, y) < 0 or min(width, height) <= 0 or x + width > 466 or y + height > 466:
        raise ValueError('面部矩形必须位于 466×466 主图内部。')

    # 先检查并转换全部输入，防止缺少素材时只更新一部分资源。
    patches = []
    for name, label in EXPRESSIONS:
        source = asset_dir / f'星璃-{label}.png'
        image = prepare_image(source)
        pixels = encode_rgb565(image.crop((x, y, x + width, y + height)))
        assert len(pixels) == width * height * 2
        patches.append((name, pixels))

    output_dir.mkdir(parents=True, exist_ok=True)
    header = '''// 自动生成，请通过 scripts/build_badge_expressions.py 更新。
#pragma once
#include "lvgl.h"
#include "sdkconfig.h"

#define ANIME_BADGE_FACE_X {x}
#define ANIME_BADGE_FACE_Y {y}
#define ANIME_BADGE_FACE_WIDTH {width}
#define ANIME_BADGE_FACE_HEIGHT {height}

#if CONFIG_ANIME_BADGE_DESKTOP
#ifdef __cplusplus
extern "C" {{
#endif
'''.format(x=x, y=y, width=width, height=height)
    for name, _ in patches:
        header += f'LV_IMAGE_DECLARE(anime_badge_{name}_patch);\n'
    header += '''#ifdef __cplusplus
}
#endif
#endif
'''

    source = '''// 自动生成，请通过 scripts/build_badge_expressions.py 更新。
// 仅存储面部矩形，矩形之外由原角色主图显示，运行时无需 PNG 解码。
#include "anime_badge_expressions.h"
#if CONFIG_ANIME_BADGE_DESKTOP
'''
    for name, pixels in patches:
        symbol = f'anime_badge_{name}_patch'
        rows = [', '.join(f'0x{byte:02x}' for byte in pixels[i:i + 24])
                for i in range(0, len(pixels), 24)]
        source += f'static const uint8_t {symbol}_pixels[] = {{\n'
        source += ',\n'.join(rows)
        source += f'''\n}};
const lv_image_dsc_t {symbol} = {{
    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0, .w = {width}, .h = {height}, .stride = {width * 2} }},
    .data_size = sizeof({symbol}_pixels), .data = {symbol}_pixels,
}};
'''
        print(f'{symbol}: {width}×{height}, RGB565, {len(pixels):,} 字节')
    source += '#endif\n'
    (output_dir / 'anime_badge_expressions.h').write_text(header, encoding='utf-8')
    (output_dir / 'anime_badge_expressions.c').write_text(source, encoding='utf-8')
    print(f'表情像素合计：{sum(len(pixels) for _, pixels in patches):,} 字节')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset-dir', type=Path, default=PROJECT_ROOT / 'badge-assets')
    parser.add_argument('--output-dir', type=Path, default=BOARD_DIRECTORY)
    parser.add_argument('--rect', type=int, nargs=4, metavar=('X', 'Y', 'WIDTH', 'HEIGHT'),
                        default=(120, 138, 216, 162), help='在 466×466 主图上的面部矩形')
    args = parser.parse_args()
    build_expressions(args.asset_dir, args.output_dir, args.rect)
