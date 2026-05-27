# Loop Run-Limit Modes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add time-duration and repeat-count run limits to Macro Runner loops, alongside the existing run-once and repeat-continuously modes.

**Architecture:** Extract the pure termination logic and legacy-config mapping into a new GUI-free module `companion-app/loop_logic.py` so it can be unit-tested without PyQt6 or the `keyboard` library. Wire those helpers into `macro_runner.py`: a four-radio Mode control with count and duration inputs, a richer per-loop config dict (backward compatible with the old `repeat` bool), and a `LoopPlayer` that checks the termination condition at each iteration boundary.

**Tech Stack:** Python 3.13 (`py` launcher on Windows), PyQt6, `keyboard`, pytest 9.

---

## File Structure

- **Create** `companion-app/loop_logic.py` — three pure functions: `duration_to_seconds`, `loop_should_continue`, `normalize_mode_config`. No PyQt6/keyboard imports.
- **Create** `companion-app/conftest.py` — puts `companion-app/` on `sys.path` so tests can `import loop_logic`.
- **Create** `companion-app/tests/test_loop_logic.py` — unit tests for the three helpers.
- **Modify** `companion-app/macro_runner.py` — import helpers + `time`; update `LoopPlayer`, `LoopCard` (UI + persistence), `MainWindow._play_card`; add `_mode_suffix`.
- **Modify** `README.md` — usage step 3 describes all four modes.

Run all test commands from `c:/Users/janhe/OneDrive/Documents/VS Projects/Bluetooth-Foot-Pedals-master/companion-app`.

---

## Task 1: Pure loop-logic helpers (TDD)

**Files:**
- Create: `companion-app/conftest.py`
- Create: `companion-app/tests/test_loop_logic.py`
- Create: `companion-app/loop_logic.py`

- [ ] **Step 1: Create conftest so tests can import the module**

Create `companion-app/conftest.py`:

```python
import os
import sys

# Allow `import loop_logic` from tests without packaging the app.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
```

- [ ] **Step 2: Write the failing tests**

Create `companion-app/tests/test_loop_logic.py`:

```python
from loop_logic import (
    duration_to_seconds,
    loop_should_continue,
    normalize_mode_config,
)


# --- duration_to_seconds ---

def test_duration_seconds_unit():
    assert duration_to_seconds(30, "seconds") == 30


def test_duration_minutes_unit():
    assert duration_to_seconds(5, "minutes") == 300


def test_duration_unknown_unit_treated_as_seconds():
    assert duration_to_seconds(10, "bogus") == 10


# --- loop_should_continue (evaluated AFTER a full pass completes) ---

def test_once_never_continues():
    assert loop_should_continue("once", 1, 10, 0.0, 0.0) is False


def test_continuous_always_continues():
    assert loop_should_continue("continuous", 999, 0, 0.0, 0.0) is True


def test_count_continues_until_reached():
    assert loop_should_continue("count", 1, 3, 0.0, 0.0) is True
    assert loop_should_continue("count", 2, 3, 0.0, 0.0) is True
    assert loop_should_continue("count", 3, 3, 0.0, 0.0) is False


def test_duration_continues_until_limit():
    assert loop_should_continue("duration", 1, 0, 4.0, 5.0) is True
    assert loop_should_continue("duration", 1, 0, 5.0, 5.0) is False
    assert loop_should_continue("duration", 1, 0, 6.0, 5.0) is False


def test_unknown_mode_stops():
    assert loop_should_continue("bogus", 1, 10, 0.0, 0.0) is False


# --- normalize_mode_config ---

def test_legacy_repeat_true_to_continuous():
    assert normalize_mode_config({"repeat": True})["mode"] == "continuous"


def test_legacy_repeat_false_to_once():
    assert normalize_mode_config({"repeat": False})["mode"] == "once"


def test_legacy_missing_repeat_defaults_continuous():
    assert normalize_mode_config({})["mode"] == "continuous"


def test_explicit_mode_preserved():
    out = normalize_mode_config({"mode": "count", "repeat_count": 7})
    assert out["mode"] == "count"
    assert out["repeat_count"] == 7


def test_defaults_filled():
    out = normalize_mode_config({"mode": "duration"})
    assert out["repeat_count"] == 10
    assert out["duration_value"] == 5
    assert out["duration_unit"] == "minutes"


def test_invalid_mode_falls_back_to_legacy():
    out = normalize_mode_config({"mode": "bogus", "repeat": False})
    assert out["mode"] == "once"


def test_invalid_unit_falls_back_to_minutes():
    out = normalize_mode_config({"mode": "duration", "duration_unit": "hours"})
    assert out["duration_unit"] == "minutes"
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `py -m pytest tests/test_loop_logic.py -v`
Expected: collection/import error — `ModuleNotFoundError: No module named 'loop_logic'`.

- [ ] **Step 4: Implement the helpers**

Create `companion-app/loop_logic.py`:

```python
"""Pure, GUI-free helpers for Macro Runner loop run-limit logic.

Kept separate from macro_runner.py (which imports PyQt6 and `keyboard`) so the
termination logic and legacy-config mapping are unit-testable without a display
or keyboard hook.
"""

VALID_MODES = ("once", "continuous", "count", "duration")
VALID_UNITS = ("seconds", "minutes")

DEFAULT_MODE = "continuous"
DEFAULT_REPEAT_COUNT = 10
DEFAULT_DURATION_VALUE = 5
DEFAULT_DURATION_UNIT = "minutes"


def duration_to_seconds(value, unit):
    """Convert a duration value + unit to seconds.

    'minutes' multiplies by 60; anything else is treated as seconds.
    """
    if unit == "minutes":
        return value * 60
    return value


def loop_should_continue(mode, completed_passes, repeat_count,
                         elapsed_seconds, duration_seconds):
    """Decide whether to run another pass, evaluated AFTER a full pass completes.

    once       -> never (a single pass only)
    continuous -> always
    count      -> until completed_passes reaches repeat_count
    duration   -> until elapsed_seconds reaches duration_seconds
    unknown    -> stop (safe default)
    """
    if mode == "continuous":
        return True
    if mode == "count":
        return completed_passes < repeat_count
    if mode == "duration":
        return elapsed_seconds < duration_seconds
    # "once" and any unrecognised mode
    return False


def normalize_mode_config(cfg):
    """Return {mode, repeat_count, duration_value, duration_unit} from a loop
    config dict.

    Maps the legacy boolean 'repeat' field onto the new modes and fills defaults
    for any missing or invalid values, so old macro_settings.json files load
    unchanged.
    """
    mode = cfg.get("mode")
    if mode not in VALID_MODES:
        # Legacy config: derive from the old 'repeat' bool (default True).
        mode = "continuous" if cfg.get("repeat", True) else "once"
    unit = cfg.get("duration_unit", DEFAULT_DURATION_UNIT)
    if unit not in VALID_UNITS:
        unit = DEFAULT_DURATION_UNIT
    return {
        "mode": mode,
        "repeat_count": cfg.get("repeat_count", DEFAULT_REPEAT_COUNT),
        "duration_value": cfg.get("duration_value", DEFAULT_DURATION_VALUE),
        "duration_unit": unit,
    }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `py -m pytest tests/test_loop_logic.py -v`
Expected: PASS (15 passed).

- [ ] **Step 6: Commit**

```bash
git add companion-app/loop_logic.py companion-app/conftest.py companion-app/tests/test_loop_logic.py
git commit -m "Add pure loop run-limit helpers with tests"
```

---

## Task 2: Integrate run-limit modes into macro_runner.py

This task touches several regions of one tightly-coupled file. The app is not runnable until all edits are done, so there is a single smoke check and commit at the end.

**Files:**
- Modify: `companion-app/macro_runner.py`

- [ ] **Step 1: Add `time` and loop_logic imports**

In `companion-app/macro_runner.py`, change the import block.

Find:

```python
import sys
import os
import json
import threading
```

Replace with:

```python
import sys
import os
import json
import threading
import time
```

Then find:

```python
import keyboard
```

Replace with:

```python
import keyboard

from loop_logic import duration_to_seconds, loop_should_continue, normalize_mode_config
```

- [ ] **Step 2: Update `LoopPlayer.__init__` for the new mode params**

Find:

```python
    def __init__(self, steps, repeat):
        super().__init__()
        # Pre-compute (action, combo, duration_seconds) so we touch no GUI in run().
        self._plan = [
            (s.get("action", "tap"),
             step_to_combo(s["mods"], s["key"]),
             max(10, s["delay"]) / 1000.0)
            for s in steps
        ]
        self._repeat = repeat
        self._stop = threading.Event()
```

Replace with:

```python
    def __init__(self, steps, mode, repeat_count, duration_seconds):
        super().__init__()
        # Pre-compute (action, combo, duration_seconds) so we touch no GUI in run().
        self._plan = [
            (s.get("action", "tap"),
             step_to_combo(s["mods"], s["key"]),
             max(10, s["delay"]) / 1000.0)
            for s in steps
        ]
        self._mode = mode
        self._repeat_count = repeat_count
        self._duration_seconds = duration_seconds
        self._stop = threading.Event()
```

- [ ] **Step 3: Update `LoopPlayer.run` to check the termination condition per pass**

Find:

```python
        try:
            while not self._stop.is_set():
                for action, combo, dur in self._plan:
                    if self._stop.is_set():
                        break
                    if action == "hold" and combo:
                        # Press, hold for the duration, then always release —
                        # even if Stop fires mid-hold, so no key stays stuck down.
                        try:
                            keyboard.press(combo)
                        except Exception as e:
                            print(f"press error for '{combo}': {e}")
                        interrupted = self._stop.wait(dur)
                        try:
                            keyboard.release(combo)
                        except Exception as e:
                            print(f"release error for '{combo}': {e}")
                        if interrupted:
                            break
                    elif action == "tap" and combo:
                        try:
                            keyboard.send(combo)
                        except Exception as e:
                            print(f"send error for '{combo}': {e}")
                        if self._stop.wait(dur):
                            break
                    else:
                        # Wait, or a key-less tap/hold: just pause for the duration.
                        if self._stop.wait(dur):
                            break
                if not self._repeat:
                    break
        finally:
            self.finished_playing.emit()
```

Replace with:

```python
        start = time.monotonic()
        completed = 0
        try:
            while not self._stop.is_set():
                for action, combo, dur in self._plan:
                    if self._stop.is_set():
                        break
                    if action == "hold" and combo:
                        # Press, hold for the duration, then always release —
                        # even if Stop fires mid-hold, so no key stays stuck down.
                        try:
                            keyboard.press(combo)
                        except Exception as e:
                            print(f"press error for '{combo}': {e}")
                        interrupted = self._stop.wait(dur)
                        try:
                            keyboard.release(combo)
                        except Exception as e:
                            print(f"release error for '{combo}': {e}")
                        if interrupted:
                            break
                    elif action == "tap" and combo:
                        try:
                            keyboard.send(combo)
                        except Exception as e:
                            print(f"send error for '{combo}': {e}")
                        if self._stop.wait(dur):
                            break
                    else:
                        # Wait, or a key-less tap/hold: just pause for the duration.
                        if self._stop.wait(dur):
                            break
                # A full pass finished. Decide at this boundary whether to do
                # another — so the current pass always completes ("finish current
                # iteration"), and the duration limit is a floor, not a hard cap.
                completed += 1
                if self._stop.is_set():
                    break
                if not loop_should_continue(
                    self._mode, completed, self._repeat_count,
                    time.monotonic() - start, self._duration_seconds,
                ):
                    break
        finally:
            self.finished_playing.emit()
```

- [ ] **Step 4: Replace the two-radio Mode row with the four-mode control**

Find:

```python
        # mode
        mode_row = QHBoxLayout()
        mode_row.addWidget(QLabel("Mode:"))
        self.repeat_radio = QRadioButton("Repeat continuously")
        self.once_radio = QRadioButton("Run once")
        self.repeat_radio.setChecked(True)
        grp = QButtonGroup(self)
        grp.addButton(self.repeat_radio)
        grp.addButton(self.once_radio)
        mode_row.addWidget(self.repeat_radio)
        mode_row.addWidget(self.once_radio)
        mode_row.addStretch()
        layout.addLayout(mode_row)
```

Replace with:

```python
        # mode — four mutually-exclusive options across two rows
        self.mode_group = QButtonGroup(self)
        self.once_radio = QRadioButton("Run once")
        self.continuous_radio = QRadioButton("Repeat continuously")
        self.count_radio = QRadioButton("Repeat")
        self.duration_radio = QRadioButton("Repeat for")
        for rb in (self.once_radio, self.continuous_radio,
                   self.count_radio, self.duration_radio):
            self.mode_group.addButton(rb)
        self.continuous_radio.setChecked(True)

        self.count_spin = QSpinBox()
        self.count_spin.setRange(1, 100000)
        self.count_spin.setValue(10)

        self.duration_spin = QSpinBox()
        self.duration_spin.setRange(1, 100000)
        self.duration_spin.setValue(5)
        self.unit_combo = QComboBox()
        self.unit_combo.addItems(["Seconds", "Minutes"])
        self.unit_combo.setCurrentText("Minutes")

        mode_row1 = QHBoxLayout()
        mode_row1.addWidget(QLabel("Mode:"))
        mode_row1.addWidget(self.once_radio)
        mode_row1.addWidget(self.continuous_radio)
        mode_row1.addStretch()
        layout.addLayout(mode_row1)

        mode_row2 = QHBoxLayout()
        mode_row2.addSpacing(40)
        mode_row2.addWidget(self.count_radio)
        mode_row2.addWidget(self.count_spin)
        mode_row2.addWidget(QLabel("times"))
        mode_row2.addSpacing(20)
        mode_row2.addWidget(self.duration_radio)
        mode_row2.addWidget(self.duration_spin)
        mode_row2.addWidget(self.unit_combo)
        mode_row2.addStretch()
        layout.addLayout(mode_row2)

        self.mode_group.buttonToggled.connect(self._on_mode_changed)
        self._on_mode_changed()
```

- [ ] **Step 5: Add the mode helper methods**

In `LoopCard`, immediately after the `__init__` method ends (before the `# --- steps ---` comment and `_add_step`), add:

```python
    # --- mode ---
    def _on_mode_changed(self, *args):
        # Only the selected mode's inputs are editable.
        self.count_spin.setEnabled(self.count_radio.isChecked())
        self.duration_spin.setEnabled(self.duration_radio.isChecked())
        self.unit_combo.setEnabled(self.duration_radio.isChecked())

    def _current_mode(self):
        if self.once_radio.isChecked():
            return "once"
        if self.count_radio.isChecked():
            return "count"
        if self.duration_radio.isChecked():
            return "duration"
        return "continuous"
```

- [ ] **Step 6: Update `LoopCard.get_config` to emit the new fields**

Find:

```python
    def get_config(self):
        return {
            "name": self.name_edit.text(),
            "hotkey": self.hotkey,
            "repeat": self.repeat_radio.isChecked(),
            "steps": [s.get_config() for s in self.step_widgets],
        }
```

Replace with:

```python
    def get_config(self):
        return {
            "name": self.name_edit.text(),
            "hotkey": self.hotkey,
            "mode": self._current_mode(),
            "repeat_count": self.count_spin.value(),
            "duration_value": self.duration_spin.value(),
            "duration_unit": self.unit_combo.currentText().lower(),
            "steps": [s.get_config() for s in self.step_widgets],
        }
```

- [ ] **Step 7: Update `LoopCard.set_config` to restore mode (with legacy mapping)**

Find:

```python
    def set_config(self, cfg):
        self.name_edit.setText(cfg.get("name", "Loop"))
        self.hotkey = cfg.get("hotkey", "")
        self.hotkey_field.setText(self.hotkey)
        repeat = cfg.get("repeat", True)
        self.repeat_radio.setChecked(repeat)
        self.once_radio.setChecked(not repeat)
        for sw in self.step_widgets[:]:
            self.steps_layout.removeWidget(sw)
            sw.deleteLater()
        self.step_widgets.clear()
        for step_data in cfg.get("steps", []):
            sw = self._add_step()
            if sw is not None:
                sw.set_config(step_data)
```

Replace with:

```python
    def set_config(self, cfg):
        self.name_edit.setText(cfg.get("name", "Loop"))
        self.hotkey = cfg.get("hotkey", "")
        self.hotkey_field.setText(self.hotkey)
        m = normalize_mode_config(cfg)
        self.count_spin.setValue(m["repeat_count"])
        self.duration_spin.setValue(m["duration_value"])
        ui = self.unit_combo.findText(m["duration_unit"].capitalize())
        self.unit_combo.setCurrentIndex(ui if ui >= 0 else self.unit_combo.findText("Minutes"))
        radios = {
            "once": self.once_radio,
            "continuous": self.continuous_radio,
            "count": self.count_radio,
            "duration": self.duration_radio,
        }
        radios.get(m["mode"], self.continuous_radio).setChecked(True)
        self._on_mode_changed()
        for sw in self.step_widgets[:]:
            self.steps_layout.removeWidget(sw)
            sw.deleteLater()
        self.step_widgets.clear()
        for step_data in cfg.get("steps", []):
            sw = self._add_step()
            if sw is not None:
                sw.set_config(step_data)
```

- [ ] **Step 8: Add the `_mode_suffix` status helper**

In `macro_runner.py`, after the `step_to_combo` function (just before `class LoopStepWidget(QFrame):`), add:

```python
def _mode_suffix(cfg):
    """Short parenthetical describing a loop's run limit, for the status bar."""
    mode = cfg["mode"]
    if mode == "once":
        return " (once)"
    if mode == "count":
        return f" ({cfg['repeat_count']}x)"
    if mode == "duration":
        return f" (for {cfg['duration_value']} {cfg['duration_unit']})"
    return ""  # continuous
```

- [ ] **Step 9: Update `MainWindow._play_card` to build the player from the new config**

Find:

```python
        player = LoopPlayer(cfg["steps"], cfg["repeat"])
        player.finished_playing.connect(lambda c=card: self._on_player_finished(c))
        self.players[card] = player
        card.set_running(True)
        player.start()
        self.status.showMessage(f"Playing: {cfg['name']}")
```

Replace with:

```python
        dur_s = duration_to_seconds(cfg["duration_value"], cfg["duration_unit"])
        player = LoopPlayer(cfg["steps"], cfg["mode"], cfg["repeat_count"], dur_s)
        player.finished_playing.connect(lambda c=card: self._on_player_finished(c))
        self.players[card] = player
        card.set_running(True)
        player.start()
        self.status.showMessage(f"Playing: {cfg['name']}{_mode_suffix(cfg)}")
```

- [ ] **Step 10: Import sanity check (catches syntax / missing-symbol errors without a display)**

Run: `py -c "import macro_runner; print('import ok')"`
Expected: prints `import ok` with no traceback. (A warning about no QApplication is fine; a traceback is not.)

- [ ] **Step 11: Re-run the unit tests to confirm nothing regressed**

Run: `py -m pytest tests/test_loop_logic.py -v`
Expected: PASS (15 passed).

- [ ] **Step 12: Commit**

```bash
git add companion-app/macro_runner.py
git commit -m "Add time and repeat-count run limits to Macro Runner loops"
```

---

## Task 3: Update README usage docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Rewrite usage step 3 to describe all four modes**

In `README.md`, find:

```markdown
3. Choose **Repeat continuously** or **Run once**.
```

Replace with:

```markdown
3. Choose a **Mode**:
   - **Run once** — play the steps a single time, then stop.
   - **Repeat continuously** — repeat until you stop it.
   - **Repeat _N_ times** — repeat the whole sequence a set number of times.
   - **Repeat for** a number of **Seconds**/**Minutes** — repeat until the time
     is up. The pass in progress always finishes, so a run can slightly exceed
     the limit.
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "Document loop run-limit modes in README"
```

---

## Task 4: Manual verification matrix

No code. Launch the app and confirm each behavior. (Use the in-app Play/Stop buttons; the global hotkey is unchanged by this work.)

**Files:** none

- [ ] **Step 1: Launch the app**

Run (from `companion-app`): `py macro_runner.py`

- [ ] **Step 2: Mode input enable/disable**

Build a loop with one **Wait** step (e.g. 500 ms). Click each Mode radio in turn and confirm:
- "Run once" / "Repeat continuously": count box, duration box, and unit dropdown all greyed out.
- "Repeat": only the count box is editable.
- "Repeat for": only the duration box and unit dropdown are editable.

- [ ] **Step 3: Count mode stops after N passes**

Set mode "Repeat" = 3, one Wait step of 300 ms. Click Play. Confirm the status bar shows `Playing: Loop 1 (3x)` and the loop auto-stops (Play re-enables) after ~0.9 s.

- [ ] **Step 4: Duration mode stops at the boundary and finishes the current pass**

Set mode "Repeat for" = 3 Seconds, one Wait step of 1000 ms. Click Play. Confirm status shows `(for 3 seconds)` and it stops after a whole number of passes once ≥3 s elapsed (i.e. it does not cut a pass short).

- [ ] **Step 5: Run once and continuous unchanged**

"Run once": plays one pass, then stops. "Repeat continuously": runs until you click Stop, which stops it promptly.

- [ ] **Step 6: Persistence round-trip**

Set a loop to "Repeat" = 7, click **Save**, close, relaunch. Confirm the loop reloads with mode "Repeat" = 7. Repeat for a "Repeat for" = 2 Minutes loop.

- [ ] **Step 7: Legacy config compatibility**

Edit `companion-app/macro_settings.json`: in one loop object, delete the `mode`/`repeat_count`/`duration_value`/`duration_unit` keys and add `"repeat": false`. Relaunch and confirm that loop loads as **Run once**. Change to `"repeat": true` and confirm it loads as **Repeat continuously**.
