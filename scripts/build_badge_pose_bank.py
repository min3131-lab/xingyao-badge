#!/usr/bin/env python3
"""把现有头部资源预旋转为 LZ4 姿态库；设备只解压选中的姿态。"""
import argparse
import array
import ctypes
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

from PIL import Image

from convert_badge_portrait import encode_rgb565, prepare_image


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BOARD_DIRECTORY = PROJECT_ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75'
WIDTH, SOURCE_HEIGHT, HEIGHT, OPAQUE_HEIGHT = 466, 350, 400, 224
PIVOT = (233, 320)
UPPER_SIZE = WIDTH * OPAQUE_HEIGHT * 2
LOWER_RGB_SIZE = WIDTH * (HEIGHT - OPAQUE_HEIGHT) * 2
RAW_SIZE = UPPER_SIZE + WIDTH * (HEIGHT - OPAQUE_HEIGHT) * 3
MAGIC = b'BDGPOSE1'
HEADER = struct.Struct('<8s14I')
ENTRY = struct.Struct('<iIIIII')


class LvglLz4:
    """在临时目录编译工程已附带的官方 LZ4，不另装压缩库或改写算法。"""

    def __init__(self, directory):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            raise RuntimeError('需要本机 C 编译器来使用 LVGL 自带的 LZ4。')
        source = PROJECT_ROOT / 'managed_components/lvgl__lvgl/src/libs/lz4/lz4.c'
        if not source.is_file():
            raise FileNotFoundError('请先解析工程依赖，确保 LVGL 的 lz4.c 已下载。')
        shim = directory / 'lvgl_memory_shim.c'
        shim.write_text('''// 主机诊断适配层：仅把 LVGL 内存接口映射到标准 C 库。
#include <string.h>
#include <stdint.h>
void* lv_memcpy(void* d, const void* s, size_t n) { return memcpy(d, s, n); }
void lv_memset(void* d, uint8_t v, size_t n) { memset(d, v, n); }
void* lv_memmove(void* d, const void* s, size_t n) { return memmove(d, s, n); }
''', encoding='utf-8')
        library = directory / ('lvgl_lz4.dylib' if sys.platform == 'darwin' else 'lvgl_lz4.so')
        flags = ['-dynamiclib'] if sys.platform == 'darwin' else ['-shared', '-fPIC']
        subprocess.run([compiler, *flags, '-O3', '-DLV_CONF_SKIP', '-DLV_USE_LZ4=1',
                        '-DLV_USE_LZ4_INTERNAL=1', str(source), str(shim), '-o', str(library)],
                       check=True)
        self.library = ctypes.CDLL(str(library))
        self.library.LZ4_compressBound.argtypes = [ctypes.c_int]
        self.library.LZ4_compressBound.restype = ctypes.c_int
        arguments = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        self.library.LZ4_compress_default.argtypes = arguments
        self.library.LZ4_compress_default.restype = ctypes.c_int
        self.library.LZ4_decompress_safe.argtypes = arguments
        self.library.LZ4_decompress_safe.restype = ctypes.c_int

    def compress(self, data):
        output = ctypes.create_string_buffer(self.library.LZ4_compressBound(len(data)))
        size = self.library.LZ4_compress_default(data, output, len(data), len(output))
        if size <= 0:
            raise RuntimeError('LZ4 压缩失败。')
        return output.raw[:size]

    def decompress(self, data, expected_size):
        output = ctypes.create_string_buffer(expected_size)
        size = self.library.LZ4_decompress_safe(data, output, len(data), expected_size)
        if size != expected_size:
            raise ValueError(f'LZ4 解压长度错误：{size}，应为 {expected_size}。')
        return output.raw


def read_head(source):
    """读取已编入固件的 RGB565A8 字节，避免 PNG 与运行素材的量化差异。"""
    match = re.search(r'anime_badge_head_pixels\[\]\s*=\s*\{(.*?)\};',
                      source.read_text(encoding='utf-8'), re.S)
    if not match:
        raise ValueError('未找到 anime_badge_head_pixels 数组。')
    pixels = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', match.group(1)))
    if len(pixels) != WIDTH * SOURCE_HEIGHT * 3:
        raise ValueError('头部源资源必须为 466×350 RGB565A8。')
    packed = array.array('H', pixels[:WIDTH * SOURCE_HEIGHT * 2])
    if sys.byteorder != 'little':
        packed.byteswap()
    rgb = bytearray(WIDTH * SOURCE_HEIGHT * 3)
    for index, value in enumerate(packed):
        # 与 LVGL RGB565→ARGB8888 混合器的通道展开公式保持一致。
        rgb[index * 3] = ((value >> 11) * 2106) >> 8
        rgb[index * 3 + 1] = (((value >> 5) & 63) * 1037) >> 8
        rgb[index * 3 + 2] = ((value & 31) * 2106) >> 8
    head = Image.frombytes('RGB', (WIDTH, SOURCE_HEIGHT), bytes(rgb)).convert('RGBA')
    head.putalpha(Image.frombytes('L', (WIDTH, SOURCE_HEIGHT), pixels[WIDTH * SOURCE_HEIGHT * 2:]))
    # 向下扩透明画布，原图位置和颈部轴不动，保留大角度旋转后落到 350 行以下的发梢。
    expanded = Image.new('RGBA', (WIDTH, HEIGHT), (0, 0, 0, 0))
    expanded.alpha_composite(head, (0, 0))
    return expanded, hashlib.sha256(pixels).hexdigest()


def pack_pose(head, angle_tenths):
    # LVGL 正角是顺时针；Pillow 正角是逆时针，故取负号。围绕原颈部轴变换，画布不扩展。
    # 这里只做资源预变换，不修改原角色形象；设备无需逐帧执行软件旋转。
    rotated = head.rotate(-angle_tenths / 10.0, resample=Image.Resampling.BICUBIC,
                          center=PIVOT, expand=False)
    upper = rotated.crop((0, 0, WIDTH, OPAQUE_HEIGHT))
    black = Image.new('RGBA', upper.size, (0, 0, 0, 255))
    upper = Image.alpha_composite(black, upper).convert('RGB')
    lower = rotated.crop((0, OPAQUE_HEIGHT, WIDTH, HEIGHT))
    # 近不透明像素的微小 alpha 误差不应使整个表情区域进入昂贵混合路径。
    alpha = lower.getchannel('A').point(lambda value: 255 if value >= 253 else value)
    raw = bytes(encode_rgb565(upper)) + bytes(encode_rgb565(lower.convert('RGB'))) + alpha.tobytes()
    if len(raw) != RAW_SIZE:
        raise ValueError(f'姿态字节数错误：{len(raw)}。')
    return raw


def assemble_bank(frames):
    data_offset = (HEADER.size + len(frames) * ENTRY.size + 15) & ~15
    bank = bytearray(data_offset)
    # Header64B：magic、版本、头大小、数量、宽高、分块行、raw大小、pivot、目录位置/项大小、数据位置、codec、保留。
    HEADER.pack_into(bank, 0, MAGIC, 1, HEADER.size, len(frames), WIDTH, HEIGHT, OPAQUE_HEIGHT,
                     RAW_SIZE, *PIVOT, HEADER.size, ENTRY.size, data_offset, 1, 0)
    for index, (angle, compressed, checksum) in enumerate(frames):
        # Entry24B：十分度角、文件内绝对偏移、压缩长度、解压长度、原始CRC32、保留。
        ENTRY.pack_into(bank, HEADER.size + index * ENTRY.size, angle, len(bank), len(compressed),
                        RAW_SIZE, checksum, 0)
        bank.extend(compressed)
    return bytes(bank)


def validate_bank(bank, compressor, expected_angles):
    values = HEADER.unpack_from(bank)
    magic, version, header_size, count, width, height, split, raw_size, px, py, table, entry_size, data, codec, reserved = values
    if (magic, version, header_size, width, height, split, raw_size, px, py, table, entry_size,
            codec, reserved) != (MAGIC, 1, 64, WIDTH, HEIGHT, OPAQUE_HEIGHT, RAW_SIZE, *PIVOT, 64, 24, 1, 0):
        raise ValueError('姿态库头部元数据错误。')
    if count != len(expected_angles) or data < table + count * entry_size or data % 16:
        raise ValueError('姿态目录范围或对齐错误。')
    previous_end = data
    for index, expected in enumerate(expected_angles):
        angle, offset, size, unpacked_size, checksum, entry_reserved = ENTRY.unpack_from(
            bank, table + index * entry_size)
        if angle != expected or offset != previous_end or size <= 0 or offset + size > len(bank):
            raise ValueError(f'第 {index} 项的角度或存储范围错误。')
        if unpacked_size != RAW_SIZE or entry_reserved:
            raise ValueError(f'第 {index} 项的解压长度错误。')
        raw = compressor.decompress(bank[offset:offset + size], unpacked_size)
        if zlib.crc32(raw) != checksum:
            raise ValueError(f'第 {index} 项 CRC32 错误。')
        previous_end = offset + size
    if previous_end != len(bank):
        raise ValueError('姿态库尾部存在未登记数据。')


def save_previews(head, directory, angles):
    """仅输出诊断预览；不覆盖原素材，不把预览 PNG 写入姿态库。"""
    directory.mkdir(parents=True, exist_ok=True)
    body = prepare_image(PROJECT_ROOT / 'badge-assets/星璃-身体层-v2.png').convert('RGBA')
    for angle in angles:
        frame = body.copy()
        rotated = head.rotate(-angle / 10.0, resample=Image.Resampling.BICUBIC, center=PIVOT)
        frame.alpha_composite(rotated, (0, 0))
        frame.convert('RGB').save(directory / f'pose_{angle:+04d}.png')


def build(args):
    head, source_hash = read_head(args.source)
    angles = list(range(args.min_angle * 10, args.max_angle * 10 + 1, 10))
    if not angles or len(angles) > 181:
        raise ValueError('角度范围无效。')
    with tempfile.TemporaryDirectory(prefix='badge-pose-lz4-') as temporary:
        compressor = LvglLz4(Path(temporary))
        frames = []
        for angle in angles:
            raw = pack_pose(head, angle)
            compressed = compressor.compress(raw)
            if compressor.decompress(compressed, RAW_SIZE) != raw:
                raise ValueError(f'{angle / 10:g}° 压缩往返校验失败。')
            frames.append((angle, compressed, zlib.crc32(raw)))
        bank = assemble_bank(frames)
        limit = int(args.max_mib * 1024 * 1024)
        if len(bank) > limit:
            raise ValueError(f'姿态库 {len(bank):,} 字节仍超出 {limit:,} 字节预算，未写入输出。')
        validate_bank(bank, compressor, angles)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(bank)
        # 写回后再次完整检查存储边界、CRC32 和每帧解压长度。
        validate_bank(args.output.read_bytes(), compressor, angles)
    if args.preview_dir:
        save_previews(head, args.preview_dir, [angles[0], 0, 170, angles[-1]])
    sizes = [len(frame[1]) for frame in frames]
    print(f'源头部像素 SHA256：{source_hash}')
    print(f'姿态库：{len(frames)} 档，{angles[0] / 10:g}..{angles[-1] / 10:g}°，间隔 1°')
    print(f'单帧 raw：{RAW_SIZE:,} 字节 = upper {UPPER_SIZE:,} + lower RGB {LOWER_RGB_SIZE:,} + alpha {WIDTH * (HEIGHT - OPAQUE_HEIGHT):,}')
    print(f'LZ4 每帧：最小 {min(sizes):,}，平均 {sum(sizes) / len(sizes):,.0f}，最大 {max(sizes):,} 字节')
    print(f'总大小：{len(bank):,} 字节 / {len(bank) / 1048576:.3f} MiB；所有帧往返及结构校验通过。')
    print(f'输出：{args.output}')
    print(f'姿态库 SHA256：{hashlib.sha256(bank).hexdigest()}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=BOARD_DIRECTORY / 'anime_badge_layers.c')
    parser.add_argument('--output', type=Path,
                        default=PROJECT_ROOT / 'badge-assets/runtime/badge_pose_bank.bin')
    parser.add_argument('--min-angle', type=int, default=-10)
    parser.add_argument('--max-angle', type=int, default=32)
    parser.add_argument('--max-mib', type=float, default=7.6)
    parser.add_argument('--preview-dir', type=Path)
    build(parser.parse_args())
