#!/usr/bin/env python3
"""将原创桌面背景和代码绘制的轻量图标打包为可直接从 Flash 绘制的素材。"""
from pathlib import Path
import math

from PIL import Image, ImageDraw

from convert_badge_portrait import encode_rgb565

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / 'badge-assets/desktop'
SCALE = 4


def xy(points):
    return [(round(x * SCALE), round(y * SCALE)) for x, y in points]


def star(cx, cy, outer, inner, count=5, angle=-math.pi / 2):
    return [(cx + (outer if i % 2 == 0 else inner) * math.cos(angle + i * math.pi / count),
             cy + (outer if i % 2 == 0 else inner) * math.sin(angle + i * math.pi / count))
            for i in range(count * 2)]


def icon(kind, top, bottom):
    """图标由基本矢量形状绘制；角色与表情原画不经过重绘。"""
    size = 112 * SCALE
    mask = Image.new('L', (size, size))
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle((4*SCALE, 4*SCALE, 108*SCALE, 108*SCALE), 30*SCALE, fill=255)
    image = Image.new('RGBA', (size, size))
    pixels = image.load()
    for y in range(size):
        t = y / (size - 1)
        for x in range(size):
            # 左上柔光与深色底部烘焙进素材，设备无需实时渐变或阴影图层。
            glow = max(0, 1 - math.hypot(x/size - .25, y/size - .16) / .75) * 15
            rgb = tuple(min(255, round(a*(1-t)+b*t+glow)) for a, b in zip(top, bottom))
            pixels[x, y] = (*rgb, 255)
    image.putalpha(mask)
    d = ImageDraw.Draw(image)
    d.rounded_rectangle((4*SCALE, 4*SCALE, 108*SCALE, 108*SCALE), 30*SCALE,
                        outline=(210, 194, 255, 165), width=SCALE)
    cream = '#fff2ff'
    pink = '#ffc1df'
    cyan = '#b4f0f9'
    if kind == 'chat':
        d.rounded_rectangle((25*SCALE, 29*SCALE, 87*SCALE, 77*SCALE), 18*SCALE,
                            fill='#ffdeef', outline=cream, width=SCALE)
        d.polygon(xy([(35, 73), (33, 88), (52, 75)]), fill='#ffdeef')
        heart = [(56 + 14*math.sin(t)**3,
                  53 - (11*math.cos(t)-4.5*math.cos(2*t)-2*math.cos(3*t)-math.cos(4*t)))
                 for t in [i*math.tau/120 for i in range(121)]]
        d.polygon(xy(heart), fill='#af5d91')
        d.polygon(xy(star(85, 24, 6, 2, 4)), fill=cyan)
    elif kind == 'settings':
        for x, y in [(34, 46), (56, 68), (78, 39)]:
            d.line(xy([(x, 30), (x, 83)]), fill='#c5b4e9', width=3*SCALE)
            d.ellipse(((x-6)*SCALE, (y-6)*SCALE, (x+6)*SCALE, (y+6)*SCALE), fill=cream)
            d.ellipse(((x-2)*SCALE, (y-2)*SCALE, (x+2)*SCALE, (y+2)*SCALE), fill='#8669b2')
        d.polygon(xy(star(88, 83, 5, 1.7, 4)), fill=pink)
    else:
        # 两段圆弧构成真正透明的月牙切口，背景无需颜色覆盖补丁。
        moon = Image.new('RGBA', (size, size))
        m = ImageDraw.Draw(moon)
        m.ellipse((28*SCALE, 25*SCALE, 84*SCALE, 83*SCALE), fill='#ede2ff')
        m.ellipse((47*SCALE, 16*SCALE, 96*SCALE, 66*SCALE), fill=(0, 0, 0, 0))
        image.alpha_composite(moon)
        d = ImageDraw.Draw(image)
        d.polygon(xy(star(76, 41, 9, 2.8, 4)), fill=cyan)
        d.polygon(xy(star(87, 69, 4, 1.5, 4)), fill=pink)
    return image.resize((112, 112), Image.Resampling.LANCZOS)


def main():
    # 背景仅缩放和转换像素格式；图像生成器输出原件完整保留。
    background = Image.open(ART / '星瑶-星空背景.png').convert('RGB').resize(
        (466, 466), Image.Resampling.LANCZOS)
    background.save(ART / 'background-466.png')
    payload = bytearray(b'BDESK001')
    payload.extend(encode_rgb565(background))
    specs = [('chat', (132, 72, 114), (67, 39, 73)),
             ('settings', (86, 83, 138), (40, 42, 78)),
             ('sleep', (67, 93, 127), (32, 43, 72))]
    for kind, top, bottom in specs:
        art = icon(kind, top, bottom)
        art.save(ART / f'{kind}.png')
        # LVGL RGB565A8 为 RGB565 平面后接独立 alpha 平面。
        payload.extend(encode_rgb565(art.convert('RGB')))
        payload.extend(art.getchannel('A').tobytes())
    assert len(payload) == 8 + 466*466*2 + 3*112*112*3
    output = ROOT / 'badge-assets/runtime/badge_desktop.bin'
    output.write_bytes(payload)
    print(f'{output}: {len(payload):,} 字节，运行时不分配像素缓存')


if __name__ == '__main__':
    main()
