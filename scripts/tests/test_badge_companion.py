"""在主机编译同一份养成模型，验证交易、冷却、计时和传感器误触边界。"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class CompanionTests(unittest.TestCase):
    def test_state_and_sensor_rules(self):
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler)
        code = r'''
#include "badge_companion.h"
#include <cassert>
#include <cmath>
int main() {
    BadgeCompanion m;
    assert(m.state.coins == 0 && m.Feed(0) == 0);
    assert(m.Touch(false));
    assert(m.state.coins == 5 && !m.Touch(true));
    assert(m.Feed(0) == 1 && m.state.coins == 0 && m.state.fullness == 76);
    assert(m.Feed(0) == 0 && m.state.fullness == 76);
    m.Tick(299, 0); assert(!m.Touch(false));
    m.Tick(1, 0); assert(m.Touch(false));
    auto snapshot = m.state;
    BadgeCompanion restored; assert(restored.Restore(snapshot));
    assert(!restored.Touch(false) && restored.state.coins == 5);
    snapshot.version = 12; assert(!restored.Restore(snapshot));
    m.state.fullness = 99; auto coins = m.state.coins;
    assert(m.Feed(0) == -1 && m.state.coins == coins);
    m.FocusSelect(15); m.FocusToggle();
    m.Tick(200, 0); assert(m.focus_left == 700);
    m.FocusToggle(); m.Tick(500, 0); assert(m.focus_left == 700);
    m.FocusToggle(); auto before = m.state.coins;
    assert(m.Tick(700, 0) & BadgeCompanion::FocusDone);
    assert(m.state.focus_minutes == 15 && m.state.coins >= before + 6);
    assert(!(m.Tick(1, 0) & BadgeCompanion::FocusDone));
    BadgeCompanion walk;
    for(unsigned step=1; step<=100; step++) walk.Tick(1,step);
    assert(walk.state.day_steps == 100 && walk.state.coins == 5);
    walk.Tick(1, 100); assert(walk.state.coins == 5);
    walk.Tick(1, 0); assert(walk.state.day_steps == 100);
    walk.Tick(1, 999999); assert(walk.state.day_steps == 100);
    // 仅倾斜时重力模长不变，静止噪声和孤立摇晃都不能产生步数。
    BadgeStepDetector still; unsigned total=0;
    for(unsigned t=0;t<30000;t+=20) total += still.Sample(1+0.015f*std::sin(t*0.01f),t);
    assert(total==0);
    BadgeStepDetector isolated;
    for(unsigned t=0;t<10000;t+=20) {
        float a = (t>2000 && t<2800) ? 1+0.25f*std::sin(t*0.01f) : 1;
        total += isolated.Sample(a,t);
    }
    assert(total==0);
    BadgeStepDetector steps; total=0;
    for(unsigned t=0;t<10000;t+=20) total += steps.Sample(1+0.25f*std::sin(t*0.01256637f),t);
    assert(total>=17 && total<=21);
    assert(walk.Mood() == std::string("安心"));
    BadgeCompanion reminders;
    reminders.state.water=reminders.state.rest=1;
    auto flags=reminders.Tick(3600,0);
    assert((flags & BadgeCompanion::Water) && (flags & BadgeCompanion::Rest));
    assert(!(reminders.Tick(1,0) & BadgeCompanion::Water));
    BadgeCompanion day;
    day.Tick(1,0,20000); day.Touch(false); day.state.walk_awards=20;
    day.Tick(1,0,20001); assert(day.state.walk_awards==0 && day.state.touch_awards==0);
    day.state.walk_awards=10; day.Tick(1,0,19999); assert(day.state.walk_awards==10);
}
'''.replace('#include <cmath>', '#include <cmath>\n#include <string>')
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder)
            (path / 'test.cc').write_text(code)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'main/boards/waveshare/esp32-s3-touch-amoled-1.75'),
                            str(path / 'test.cc'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)
