"""直接编译生产状态机，回归徽章离线启动及普通板型的状态约束。"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]

# 主机只替换 ESP 日志和构建配置，状态转换规则仍来自生产 .cc 文件。
DRIVER = r'''
#include "device_state_machine.h"
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    DeviceStateMachine machine;
    DeviceState expected_from = kDeviceStateUnknown;
    DeviceState expected_to = kDeviceStateUnknown;
    int callbacks = 0;
    machine.AddStateChangeListener([&](DeviceState from, DeviceState to) {
        // 回调必须看到已经提交的新状态，而且不能收到其他转换的通知。
        if (from != expected_from || to != expected_to || machine.GetState() != to) {
            std::cerr << "状态回调与实际转换不一致\n";
            std::exit(2);
        }
        ++callbacks;
    });
    for (int argument = 1; argument < argc; ++argument) {
        int value = kDeviceStateUnknown;
        for (; value <= kDeviceStateFatalError; ++value) {
            if (argv[argument] == std::string(DeviceStateMachine::GetStateName(
                                      static_cast<DeviceState>(value)))) {
                break;
            }
        }
        if (value > kDeviceStateFatalError) {
            std::cerr << "无法识别测试状态: " << argv[argument] << '\n';
            return 3;
        }
        expected_from = machine.GetState();
        expected_to = static_cast<DeviceState>(value);
        const bool allowed = machine.CanTransitionTo(expected_to);
        const bool changed = machine.TransitionTo(expected_to);
        std::cout << DeviceStateMachine::GetStateName(expected_from) << ' '
                  << argv[argument] << ' ' << allowed << ' ' << changed << ' '
                  << DeviceStateMachine::GetStateName(machine.GetState()) << ' '
                  << callbacks << '\n';
    }
}
'''


class DeviceStateMachineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # HOST_CXX 可指定本机工具链，避免 ESP-IDF 的 CXX 变量选中交叉编译器。
        compiler = shlex.split(os.environ.get("HOST_CXX", ""))
        if not compiler:
            candidate = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
            if not candidate:
                raise RuntimeError("状态机回归测试需要本机 C++ 编译器。")
            compiler = [candidate]
        cls.temporary = tempfile.TemporaryDirectory(prefix="badge-state-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        driver = directory / "driver.cc"
        driver.write_text(DRIVER, encoding="utf-8")
        cls.binaries = {}
        for badge in (False, True):
            variant = directory / ("badge" if badge else "ordinary")
            variant.mkdir()
            # 普通板型使用未定义宏，和关闭 Kconfig 选项时的头文件行为一致。
            (variant / "sdkconfig.h").write_text(
                "#pragma once\n" + ("#define CONFIG_ANIME_BADGE_DESKTOP 1\n" if badge else ""),
                encoding="utf-8",
            )
            (variant / "esp_log.h").write_text(
                "#pragma once\n"
                "#define ESP_LOGI(tag, ...) ((void)(tag))\n"
                "#define ESP_LOGW(tag, ...) ((void)(tag))\n",
                encoding="utf-8",
            )
            binary = variant / "state_machine_test"
            command = compiler + [
                "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-I", str(variant), "-I", str(ROOT / "main"),
                str(ROOT / "main/device_state_machine.cc"), str(driver), "-o", str(binary),
            ]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise RuntimeError(f"编译 {variant.name} 状态机失败:\n{result.stdout}{result.stderr}")
            cls.binaries[badge] = binary

    def assert_trace(self, badge, targets, accepted):
        """同时核对查询、执行、最终状态以及成功转换才触发一次的回调。"""
        result = subprocess.run(
            [str(self.binaries[badge]), *targets], capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = result.stdout.splitlines()
        self.assertEqual(len(rows), len(targets))
        state, callbacks = "unknown", 0
        for row, target, success in zip(rows, targets, accepted):
            with self.subTest(badge=badge, before=state, target=target):
                before = state
                if success:
                    callbacks += target != state
                    state = target
                self.assertEqual(
                    row.split(),
                    [before, target, str(int(success)), str(int(success)), state, str(callbacks)],
                )

    def test_badge_offline_then_provision_activate_and_chat(self):
        targets = [
            "starting", "idle", "wifi_configuring", "idle", "activating",
            "idle", "connecting", "listening", "idle",
        ]
        self.assert_trace(True, targets, [True] * len(targets))

    def test_badge_initial_provision_can_return_to_offline_idle(self):
        self.assert_trace(True, ["starting", "wifi_configuring", "idle"], [True] * 3)

    def test_ordinary_board_rejects_offline_shortcuts_but_keeps_chat_path(self):
        targets = [
            "starting", "idle", "wifi_configuring", "idle", "activating",
            "idle", "connecting", "listening", "idle",
        ]
        self.assert_trace(False, targets, [True, False, True, False, True, True, True, True, True])

    def test_idle_shortcuts_do_not_allow_recording_during_startup_or_provisioning(self):
        for badge in (False, True):
            self.assert_trace(
                badge,
                ["starting", "listening", "connecting", "wifi_configuring", "listening", "connecting"],
                [True, False, False, True, False, False],
            )

    def test_noop_and_rejected_transition_do_not_emit_state_callbacks(self):
        self.assert_trace(
            True,
            ["starting", "idle", "idle", "starting", "unknown", "idle", "connecting", "idle"],
            [True, True, True, False, False, True, True, True],
        )


if __name__ == "__main__":
    unittest.main()
