"""PC Macro Runner — a standalone loop player.

Build loops (sequences of modifier+key+delay steps) and play them locally on
this PC. Each loop is toggled by a global hotkey AND/OR in-app Play/Stop
buttons. No ESP32 / Bluetooth hardware required — this app types the keystrokes
itself via the `keyboard` library.

Loop execution mirrors the ESP32 firmware (tickLoop): step 0 fires immediately,
then each step's delay is the wait *before* the next step; at the end a loop
either repeats from the top or stops (run once).
"""

import sys
import os
import json
import threading
import time

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QGroupBox, QLabel, QComboBox, QCheckBox, QPushButton,
    QLineEdit, QStatusBar, QSpinBox, QRadioButton, QButtonGroup,
    QScrollArea, QFrame,
)
from PyQt6.QtCore import Qt, QThread, QEvent, pyqtSignal

import keyboard

from loop_logic import duration_to_seconds, loop_should_continue, normalize_mode_config

SETTINGS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "macro_settings.json")

# Display name -> name understood by the `keyboard` library.
# Letters and digits map to themselves (lowercased); only the specials differ.
SPECIAL_KEYS = {
    "Enter": "enter", "Escape": "esc", "Backspace": "backspace", "Tab": "tab",
    "Space": "space", "Delete": "delete", "Insert": "insert",
    "Home": "home", "End": "end", "Page Up": "page up", "Page Down": "page down",
    "Up Arrow": "up", "Down Arrow": "down", "Left Arrow": "left", "Right Arrow": "right",
    "Print Screen": "print screen", "Caps Lock": "caps lock",
}

# Ordered list of display names shown in the key dropdown.
KEY_NAMES = (
    [chr(c) for c in range(ord('A'), ord('Z') + 1)]
    + [str(i) for i in range(10)]
    + [f"F{i}" for i in range(1, 25)]
    + list(SPECIAL_KEYS.keys())
)

# Modifier display name -> keyboard library name. Dict order = checkbox order.
MODIFIERS = {"Ctrl": "ctrl", "Shift": "shift", "Alt": "alt", "Win": "windows"}


def key_to_kb(name):
    """Translate a display key name to the `keyboard` library's key name."""
    if name in SPECIAL_KEYS:
        return SPECIAL_KEYS[name]
    return name.lower()


def step_to_combo(mods, key):
    """Build a 'ctrl+shift+z' style combo string from modifier names + key name."""
    if not key or key == "(None)":
        return ""
    parts = [MODIFIERS[m] for m in mods if m in MODIFIERS]
    parts.append(key_to_kb(key))
    return "+".join(parts)


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


class LoopStepWidget(QFrame):
    removed = pyqtSignal(object)

    def __init__(self, step_num=1):
        super().__init__()
        self.setFrameShape(QFrame.Shape.StyledPanel)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 2, 4, 2)

        self.step_label = QLabel(f"Step {step_num}:")
        layout.addWidget(self.step_label)

        # Action: Tap (press+release), Hold (press, wait, release), Wait (do nothing).
        self.action_combo = QComboBox()
        self.action_combo.addItems(["Tap", "Hold", "Wait"])
        self.action_combo.currentTextChanged.connect(self._on_action_changed)
        layout.addWidget(self.action_combo)

        self.mod_checks = {}
        for name in MODIFIERS:
            cb = QCheckBox(name)
            self.mod_checks[name] = cb
            layout.addWidget(cb)

        self.plus_label = QLabel("+")
        layout.addWidget(self.plus_label)

        self.key_combo = QComboBox()
        self.key_combo.setMaxVisibleItems(20)
        self.key_combo.addItem("(None)")
        for name in KEY_NAMES:
            self.key_combo.addItem(name)
        layout.addWidget(self.key_combo)

        self.dur_label = QLabel("Delay:")
        layout.addWidget(self.dur_label)
        self.delay_spin = QSpinBox()
        self.delay_spin.setRange(10, 600000)
        self.delay_spin.setValue(500)
        self.delay_spin.setSuffix(" ms")
        self.delay_spin.setSingleStep(100)
        layout.addWidget(self.delay_spin)

        rm_btn = QPushButton("✕")
        rm_btn.setFixedWidth(30)
        rm_btn.clicked.connect(lambda: self.removed.emit(self))
        layout.addWidget(rm_btn)

    def _on_action_changed(self, action):
        # Wait has no key; Tap/Hold do. The duration means different things:
        # Tap -> delay after; Hold -> how long to hold; Wait -> how long to pause.
        is_wait = action == "Wait"
        for cb in self.mod_checks.values():
            cb.setVisible(not is_wait)
        self.plus_label.setVisible(not is_wait)
        self.key_combo.setVisible(not is_wait)
        self.dur_label.setText(
            {"Tap": "Delay:", "Hold": "Hold:", "Wait": "Wait:"}.get(action, "Delay:")
        )

    def set_step_number(self, num):
        self.step_label.setText(f"Step {num}:")

    def get_config(self):
        mods = [name for name, cb in self.mod_checks.items() if cb.isChecked()]
        return {
            "action": self.action_combo.currentText().lower(),
            "mods": mods,
            "key": self.key_combo.currentText(),
            "delay": self.delay_spin.value(),
        }

    def set_config(self, cfg):
        action = cfg.get("action", "tap").capitalize()
        ai = self.action_combo.findText(action)
        self.action_combo.setCurrentIndex(ai if ai >= 0 else 0)
        self._on_action_changed(self.action_combo.currentText())
        mods = cfg.get("mods", [])
        for name, cb in self.mod_checks.items():
            cb.setChecked(name in mods)
        idx = self.key_combo.findText(cfg.get("key", "(None)"))
        self.key_combo.setCurrentIndex(idx if idx >= 0 else 0)
        self.delay_spin.setValue(cfg.get("delay", 500))


class LoopPlayer(QThread):
    """Runs one loop's steps until stopped or (run-once) finished."""
    finished_playing = pyqtSignal()

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

    def run(self):
        if not self._plan:
            self.finished_playing.emit()
            return
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

    def stop(self):
        self._stop.set()


class LoopCard(QGroupBox):
    """One configurable loop: name, hotkey, mode, steps, and play controls."""
    hotkey_changed = pyqtSignal()
    removed = pyqtSignal(object)
    play_requested = pyqtSignal(object)
    stop_requested = pyqtSignal(object)

    def __init__(self, name="Loop 1"):
        super().__init__(name)
        self.hotkey = ""
        self._capture = None
        layout = QVBoxLayout(self)

        # name + remove
        top = QHBoxLayout()
        top.addWidget(QLabel("Name:"))
        self.name_edit = QLineEdit(name)
        self.name_edit.textChanged.connect(lambda t: self.setTitle(t or "Loop"))
        top.addWidget(self.name_edit)
        top.addStretch()
        rm_loop = QPushButton("Remove Loop")
        rm_loop.clicked.connect(lambda: self.removed.emit(self))
        top.addWidget(rm_loop)
        layout.addLayout(top)

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

        # hotkey + play/stop
        ctl_row = QHBoxLayout()
        ctl_row.addWidget(QLabel("Hotkey:"))
        self.hotkey_field = QLineEdit()
        self.hotkey_field.setReadOnly(True)
        self.hotkey_field.setPlaceholderText("(none)")
        self.hotkey_field.setFixedWidth(140)
        ctl_row.addWidget(self.hotkey_field)
        self.set_hotkey_btn = QPushButton("Set Hotkey")
        self.set_hotkey_btn.clicked.connect(self._capture_hotkey)
        ctl_row.addWidget(self.set_hotkey_btn)
        self.clear_hotkey_btn = QPushButton("Clear")
        self.clear_hotkey_btn.clicked.connect(self._clear_hotkey)
        ctl_row.addWidget(self.clear_hotkey_btn)
        ctl_row.addStretch()
        self.play_btn = QPushButton("▶ Play")
        self.play_btn.clicked.connect(lambda: self.play_requested.emit(self))
        ctl_row.addWidget(self.play_btn)
        self.stop_btn = QPushButton("■ Stop")
        self.stop_btn.setEnabled(False)
        self.stop_btn.clicked.connect(lambda: self.stop_requested.emit(self))
        ctl_row.addWidget(self.stop_btn)
        layout.addLayout(ctl_row)

        # steps
        self.steps_layout = QVBoxLayout()
        layout.addLayout(self.steps_layout)
        self.step_widgets = []
        add_btn = QPushButton("+ Add Step")
        add_btn.clicked.connect(self._add_step)
        layout.addWidget(add_btn)

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

    # --- steps ---
    def _add_step(self):
        if len(self.step_widgets) >= 20:
            return
        sw = LoopStepWidget(len(self.step_widgets) + 1)
        sw.removed.connect(self._remove_step)
        self.step_widgets.append(sw)
        self.steps_layout.addWidget(sw)
        return sw

    def _remove_step(self, widget):
        self.step_widgets.remove(widget)
        self.steps_layout.removeWidget(widget)
        widget.deleteLater()
        for i, sw in enumerate(self.step_widgets):
            sw.set_step_number(i + 1)

    # --- hotkey capture ---
    def _capture_hotkey(self):
        self.set_hotkey_btn.setEnabled(False)
        self.set_hotkey_btn.setText("Press keys…")
        self._capture = HotkeyCapture()
        self._capture.captured.connect(self._on_hotkey_captured)
        self._capture.start()

    def _on_hotkey_captured(self, combo):
        self.set_hotkey_btn.setEnabled(True)
        self.set_hotkey_btn.setText("Set Hotkey")
        self._capture = None
        if combo:
            self.hotkey = combo
            self.hotkey_field.setText(combo)
            self.hotkey_changed.emit()

    def _clear_hotkey(self):
        self.hotkey = ""
        self.hotkey_field.clear()
        self.hotkey_changed.emit()

    # --- running state UI ---
    def set_running(self, running):
        self.play_btn.setEnabled(not running)
        self.stop_btn.setEnabled(running)

    # --- persistence ---
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


class HotkeyCapture(QThread):
    """Blocks on keyboard.read_hotkey() in a worker thread, then reports it."""
    captured = pyqtSignal(str)

    def run(self):
        try:
            combo = keyboard.read_hotkey(suppress=False)
        except Exception as e:
            print(f"hotkey capture error: {e}")
            combo = ""
        self.captured.emit(combo or "")


class _ToggleEvent(QEvent):
    """Carries a loop-toggle request from the keyboard thread to the GUI thread."""
    TYPE = QEvent.Type(QEvent.registerEventType())

    def __init__(self, card):
        super().__init__(_ToggleEvent.TYPE)
        self.card = card


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Macro Runner")
        self.setMinimumSize(640, 520)

        self.cards = []
        self.players = {}        # card -> LoopPlayer
        self.hotkey_handles = {} # card -> keyboard hotkey handle

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        intro = QLabel(
            "Build loops and trigger them with a global hotkey (works in any "
            "window) or the Play/Stop buttons. Tip: use the hotkey so keystrokes "
            "go to your target window — clicking Play here types into whatever "
            "window was focused before this one."
        )
        intro.setWordWrap(True)
        root.addWidget(intro)

        # scrollable list of loop cards
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.content = QWidget()
        self.cards_layout = QVBoxLayout(self.content)
        self.cards_layout.addStretch()
        self.scroll.setWidget(self.content)
        root.addWidget(self.scroll)

        # bottom buttons
        btns = QHBoxLayout()
        add_loop_btn = QPushButton("+ Add Loop")
        add_loop_btn.clicked.connect(lambda: self._add_card())
        btns.addWidget(add_loop_btn)
        btns.addStretch()
        save_btn = QPushButton("Save")
        save_btn.clicked.connect(self._save)
        btns.addWidget(save_btn)
        root.addLayout(btns)

        self.status = QStatusBar()
        self.setStatusBar(self.status)

        if not self._load():
            self._add_card()  # start with one empty loop
        self._register_hotkeys()
        self.status.showMessage("Ready.")

    # --- card management ---
    def _add_card(self, cfg=None):
        name = f"Loop {len(self.cards) + 1}"
        card = LoopCard(name)
        if cfg:
            card.set_config(cfg)
        card.removed.connect(self._remove_card)
        card.play_requested.connect(self._play_card)
        card.stop_requested.connect(self._stop_card)
        card.hotkey_changed.connect(self._register_hotkeys)
        self.cards.append(card)
        self.cards_layout.insertWidget(self.cards_layout.count() - 1, card)
        return card

    def _remove_card(self, card):
        self._stop_card(card)
        self.cards.remove(card)
        self.cards_layout.removeWidget(card)
        card.deleteLater()
        self._register_hotkeys()

    # --- play / stop ---
    def _play_card(self, card):
        if card in self.players:
            return
        cfg = card.get_config()
        if not cfg["steps"]:
            self.status.showMessage(f"{cfg['name']}: add at least one step first.")
            return
        dur_s = duration_to_seconds(cfg["duration_value"], cfg["duration_unit"])
        player = LoopPlayer(cfg["steps"], cfg["mode"], cfg["repeat_count"], dur_s)
        player.finished_playing.connect(lambda c=card: self._on_player_finished(c))
        self.players[card] = player
        card.set_running(True)
        player.start()
        self.status.showMessage(f"Playing: {cfg['name']}{_mode_suffix(cfg)}")

    def _stop_card(self, card):
        player = self.players.get(card)
        if player:
            player.stop()
            player.wait()

    def _on_player_finished(self, card):
        player = self.players.pop(card, None)
        if player:
            player.deleteLater()
        card.set_running(False)

    def _toggle_card(self, card):
        # Always runs on the GUI thread (dispatched via _ToggleEvent / button click).
        if card in self.players:
            self._stop_card(card)
        else:
            self._play_card(card)

    # --- global hotkeys ---
    def _register_hotkeys(self):
        for handle in self.hotkey_handles.values():
            try:
                keyboard.remove_hotkey(handle)
            except (KeyError, ValueError):
                pass
        self.hotkey_handles.clear()
        seen = {}
        for card in self.cards:
            hk = card.hotkey.strip()
            if not hk:
                continue
            if hk in seen:
                continue  # first card wins a duplicate hotkey
            seen[hk] = card
            try:
                handle = keyboard.add_hotkey(hk, self._make_toggle(card))
                self.hotkey_handles[card] = handle
            except Exception as e:
                print(f"failed to register hotkey '{hk}': {e}")

    def _make_toggle(self, card):
        # keyboard fires this on its own thread; bounce to the GUI thread.
        def cb():
            QApplication.instance().postEvent(self, _ToggleEvent(card))
        return cb

    def customEvent(self, event):
        if isinstance(event, _ToggleEvent):
            self._toggle_card(event.card)

    # --- persistence ---
    def _save(self):
        try:
            data = {"loops": [c.get_config() for c in self.cards]}
            with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            self.status.showMessage(f"Saved to {os.path.basename(SETTINGS_FILE)}.")
        except Exception as e:
            self.status.showMessage(f"Save failed: {e}")

    def _load(self):
        try:
            with open(SETTINGS_FILE, encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            return False
        loops = data.get("loops", [])
        if not loops:
            return False
        for cfg in loops:
            self._add_card(cfg)
        return True

    def closeEvent(self, event):
        for card in list(self.players.keys()):
            self._stop_card(card)
        for handle in self.hotkey_handles.values():
            try:
                keyboard.remove_hotkey(handle)
            except (KeyError, ValueError):
                pass
        self._save()
        event.accept()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setApplicationName("Macro Runner")
    w = MainWindow()
    w.show()
    sys.exit(app.exec())
