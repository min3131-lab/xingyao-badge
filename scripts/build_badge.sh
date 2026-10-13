#!/usr/bin/env bash
# 基于现有 ESP-IDF 环境构建徽章，英文输出路径规避 GCC 对中文 specs 路径的解析问题。
set -euo pipefail
badge_root="$(cd "$(dirname "$0")/.." && pwd)"
: "${IDF_PATH:?请先激活 ESP-IDF 环境}"
badge_build_dir="${BADGE_BUILD_DIR:-$HOME/.cache/anime-badge-build}"
export PATH="$IDF_PATH/tools:$PATH"
cd "$badge_root"
mkdir -p "$badge_build_dir"
python - "$badge_build_dir/badge.defaults" <<'PY'
import json
import re
import sys
from pathlib import Path
config = json.loads(Path('main/boards/waveshare/esp32-s3-touch-amoled-1.75/config.json').read_text())
variant = next(item for item in config['builds'] if item['name'].endswith('-anime-badge'))
Path(sys.argv[1]).write_text('\n'.join(variant['sdkconfig_append']) + '\n')
# 和上游发布脚本一致，显式应用变体，避免旧 sdkconfig 覆盖新配置。
config_path = Path('sdkconfig')
if config_path.exists():
    current = config_path.read_text()
    for option in variant['sdkconfig_append']:
        key = option.split('=', 1)[0]
        current = re.sub(rf'^(?:{key}=.*|# {key} is not set)\n?', '', current, flags=re.M)
    config_path.write_text(current.rstrip() + '\n' + '\n'.join(variant['sdkconfig_append']) + '\n')
PY
idf.py -B "$badge_build_dir" \
    -D IDF_TARGET=esp32s3 \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;$badge_build_dir/badge.defaults" \
    -D BOARD_NAME=esp32-s3-touch-amoled-1.75c-anime-badge \
    build
