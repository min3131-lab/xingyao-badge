#!/usr/bin/env python3
"""借用现有 ESP-IDF 编译参数检查非徽章分支，不改配置、不链接、不写目标文件。"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCES = (
    "main/application.cc",
    "main/audio/audio_service.cc",
    "main/display/lvgl_display/lvgl_display.cc",
    "main/boards/common/wifi_board.cc",
    "main/device_state_machine.cc",
)


def syntax_command(entry, config_directory):
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    # compile_commands 中的依赖文件和目标文件选项必须移除，不能污染正在进行的真实构建。
    filtered = []
    skip_next = False
    for argument in arguments[1:]:
        if skip_next:
            skip_next = False
        elif argument in ("-o", "-MF", "-MT", "-MQ", "-MJ"):
            skip_next = True
        elif argument in ("-c", "-MD", "-MMD", "-MP"):
            continue
        elif argument.startswith("-fdiagnostics-color="):
            continue
        else:
            filtered.append(argument)
    # 临时头放最前，且预先包含它；后续包含 sdkconfig.h 仍由同一份临时头负责。
    return [arguments[0], "-I", str(config_directory), "-include",
            str(config_directory / "sdkconfig.h"), *filtered,
            "-fsyntax-only", "-fdiagnostics-color=never"]


def check(build_directory, log_path):
    config = build_directory / "config/sdkconfig.h"
    original_config = config.read_bytes()
    database = json.loads((build_directory / "compile_commands.json").read_text(encoding="utf-8"))
    entries = {Path(item["file"]).resolve(): item for item in database}
    missing = [source for source in SOURCES if (ROOT / source).resolve() not in entries]
    if missing:
        raise RuntimeError(f"编译数据库缺少目标源文件：{', '.join(missing)}")
    report = [
        f"构建参数：{build_directory / 'compile_commands.json'}",
        f"真实 sdkconfig.h SHA256：{hashlib.sha256(original_config).hexdigest()}",
        "检查类型：ESP32-S3 编译参数，临时取消 CONFIG_ANIME_BADGE_DESKTOP，仅语法检查。",
        "此检查不代替其他板型的完整构建、链接或硬件验证。",
    ]
    failures = 0
    with tempfile.TemporaryDirectory(prefix="non-badge-syntax-") as temporary:
        directory = Path(temporary)
        (directory / "sdkconfig.h").write_bytes(
            original_config + "\n// 仅此临时配置关闭徽章模式。\n"
            "#undef CONFIG_ANIME_BADGE_DESKTOP\n".encode("utf-8")
        )
        for source in SOURCES:
            item = entries[(ROOT / source).resolve()]
            result = subprocess.run(
                syntax_command(item, directory), cwd=item["directory"],
                capture_output=True, text=True, timeout=120,
            )
            status = "PASS" if result.returncode == 0 else "FAIL"
            message = f"{status}: {source}"
            report.extend([message, result.stdout, result.stderr])
            print(message, flush=True)
            failures += result.returncode != 0
    # 若外部构建同时重新生成配置，只报告变化，绝不覆盖或恢复真实文件。
    config_changed = config.read_bytes() != original_config
    if config_changed:
        report.append("注意：运行期间真实 sdkconfig.h 被其他构建更新，请确认配置后重跑。")
    report.append(f"结果：{len(SOURCES) - failures}/{len(SOURCES)} 项通过。")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text("\n".join(line for line in report if line) + "\n", encoding="utf-8")
    print(f"日志：{log_path}")
    return bool(failures or config_changed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--log", type=Path,
                        default=ROOT / "validation/lazy-start-standard-board-compile.log")
    options = parser.parse_args()
    raise SystemExit(check(options.build_dir.resolve(), options.log.resolve()))
