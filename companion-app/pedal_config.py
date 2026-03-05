import sys
import asyncio
import json

from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QGroupBox, QLabel, QComboBox, QCheckBox, QPushButton,
    QTabWidget, QLineEdit, QStatusBar, QSpinBox, QRadioButton,
    QButtonGroup, QScrollArea, QFrame,
)
from PyQt6.QtCore import Qt, QThread, pyqtSignal

from ble_comm import BLEComm

KEY_MAP = {
    **{chr(c): ord(chr(c).lower()) for c in range(ord('A'), ord('Z') + 1)},
    **{str(i): ord(str(i)) for i in range(10)},
    **{f"F{i}": 0xC1 + i for i in range(1, 13)},
    **{f"F{i}": 0xF0 + i - 13 for i in range(13, 25)},
    "Enter": 0xB0, "Escape": 0xB1, "Backspace": 0xB2, "Tab": 0xB3,
    "Space": 0x20, "Delete": 0xD4, "Insert": 0xD1,
    "Home": 0xD2, "End": 0xD5,
    "Page Up": 0xD3, "Page Down": 0xD6,
    "Up Arrow": 0xDA, "Down Arrow": 0xD9,
    "Left Arrow": 0xD8, "Right Arrow": 0xD7,
    "Print Screen": 0xCE, "Caps Lock": 0xC1,
    # Media keys (Play/Pause, Next/Prev Track, Volume, Mute) are omitted because
    # the ESP32 BLE Keyboard library uses a separate MediaKeyReport type for them,
    # which requires a different press() overload than regular uint8_t keys.
}

KEYCODE_TO_NAME = {v: k for k, v in KEY_MAP.items()}
MODIFIER_BITS = {"Ctrl": 0x01, "Shift": 0x02, "Alt": 0x04, "Win": 0x08}
PROFILE_NAMES = ["Profile 1", "Profile 2", "Profile 3", "Profile 4"]
PEDAL_NAMES = ["Pedal 1 (Left)", "Pedal 2 (Center)", "Pedal 3 (Right)"]
NUM_LOOPS = 3


class BLEWorker(QThread):
    scan_done = pyqtSignal(list)
    connect_done = pyqtSignal(bool)
    disconnect_done = pyqtSignal()
    command_done = pyqtSignal(dict)
    error = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self.comm = BLEComm()
        self._loop = None
        self._tasks = None

    def run(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._tasks = asyncio.Queue()
        self._loop.run_until_complete(self._process_tasks())

    async def _process_tasks(self):
        while True:
            name, args = await self._tasks.get()
            if name == "quit":
                break
            try:
                if name == "scan":
                    self.scan_done.emit(await self.comm.scan(timeout=args.get("timeout", 5.0)))
                elif name == "connect":
                    self.connect_done.emit(await self.comm.connect(args["address"]))
                elif name == "disconnect":
                    await self.comm.disconnect()
                    self.disconnect_done.emit()
                elif name == "command":
                    self.command_done.emit(await self.comm.send_command(args["command"]))
            except Exception as e:
                self.error.emit(str(e))

    def enqueue(self, task_name, **kwargs):
        if self._loop and self._tasks is not None:
            self._loop.call_soon_threadsafe(self._tasks.put_nowait, (task_name, kwargs))

    def stop(self):
        if self._loop and self._tasks is not None:
            self._loop.call_soon_threadsafe(self._tasks.put_nowait, ("quit", {}))
        self.wait()


class LoopStepWidget(QFrame):
    removed = pyqtSignal(object)

    def __init__(self, step_num=1):
        super().__init__()
        self.setFrameShape(QFrame.Shape.StyledPanel)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 2, 4, 2)

        self.step_label = QLabel(f"Step {step_num}:")
        layout.addWidget(self.step_label)

        self.mod_checks = {}
        for name in MODIFIER_BITS:
            cb = QCheckBox(name)
            self.mod_checks[name] = cb
            layout.addWidget(cb)

        layout.addWidget(QLabel("+"))

        self.key_combo = QComboBox()
        self.key_combo.setMaxVisibleItems(20)
        self.key_combo.addItem("(None)")
        for name in KEY_MAP:
            self.key_combo.addItem(name)
        layout.addWidget(self.key_combo)

        layout.addWidget(QLabel("Delay:"))
        self.delay_spin = QSpinBox()
        self.delay_spin.setRange(10, 60000)
        self.delay_spin.setValue(500)
        self.delay_spin.setSuffix(" ms")
        self.delay_spin.setSingleStep(100)
        layout.addWidget(self.delay_spin)

        rm_btn = QPushButton("✕")
        rm_btn.setFixedWidth(30)
        rm_btn.clicked.connect(lambda: self.removed.emit(self))
        layout.addWidget(rm_btn)

    def set_step_number(self, num):
        self.step_label.setText(f"Step {num}:")

    def get_config(self):
        mod = 0
        for name, bit in MODIFIER_BITS.items():
            if self.mod_checks[name].isChecked():
                mod |= bit
        return {
            "mod": mod,
            "key": KEY_MAP.get(self.key_combo.currentText(), 0),
            "delay": self.delay_spin.value(),
        }

    def set_config(self, cfg):
        mod = cfg.get("mod", 0)
        for name, bit in MODIFIER_BITS.items():
            self.mod_checks[name].setChecked(bool(mod & bit))

        kc = cfg.get("key", 0)
        if kc == 0:
            self.key_combo.setCurrentIndex(0)
        else:
            idx = self.key_combo.findText(KEYCODE_TO_NAME.get(kc, ""))
            if idx >= 0:
                self.key_combo.setCurrentIndex(idx)

        self.delay_spin.setValue(cfg.get("delay", 500))


class LoopEditorWidget(QGroupBox):
    def __init__(self, title):
        super().__init__(title)
        layout = QVBoxLayout(self)

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

        self.steps_layout = QVBoxLayout()
        layout.addLayout(self.steps_layout)
        self.step_widgets = []

        add_btn = QPushButton("+ Add Step")
        add_btn.clicked.connect(self._add_step)
        layout.addWidget(add_btn)

    def _add_step(self):
        if len(self.step_widgets) >= 20:
            return
        sw = LoopStepWidget(len(self.step_widgets) + 1)
        sw.removed.connect(self._remove_step)
        self.step_widgets.append(sw)
        self.steps_layout.addWidget(sw)

    def _remove_step(self, widget):
        self.step_widgets.remove(widget)
        self.steps_layout.removeWidget(widget)
        widget.deleteLater()
        for i, sw in enumerate(self.step_widgets):
            sw.set_step_number(i + 1)

    def get_config(self):
        return {
            "repeat": self.repeat_radio.isChecked(),
            "steps": [s.get_config() for s in self.step_widgets],
        }

    def set_config(self, cfg):
        self.repeat_radio.setChecked(cfg.get("repeat", True))
        self.once_radio.setChecked(not cfg.get("repeat", True))

        for sw in self.step_widgets[:]:
            self.steps_layout.removeWidget(sw)
            sw.deleteLater()
        self.step_widgets.clear()

        for step_data in cfg.get("steps", []):
            self._add_step()
            self.step_widgets[-1].set_config(step_data)


class PedalWidget(QGroupBox):
    def __init__(self, pedal_name):
        super().__init__(pedal_name)
        main_layout = QVBoxLayout(self)

        # type selector
        type_row = QHBoxLayout()
        type_row.addWidget(QLabel("Type:"))
        self.type_combo = QComboBox()
        self.type_combo.addItems(["Key", "Loop"])
        self.type_combo.currentIndexChanged.connect(self._on_type_changed)
        type_row.addWidget(self.type_combo)
        type_row.addStretch()
        main_layout.addLayout(type_row)

        # key config panel
        self.key_widget = QWidget()
        kl = QVBoxLayout(self.key_widget)
        kl.setContentsMargins(0, 0, 0, 0)

        mod_row = QHBoxLayout()
        mod_row.addWidget(QLabel("Modifiers:"))
        self.mod_checks = {}
        for name in MODIFIER_BITS:
            cb = QCheckBox(name)
            self.mod_checks[name] = cb
            mod_row.addWidget(cb)
        mod_row.addStretch()
        kl.addLayout(mod_row)

        key_row = QHBoxLayout()
        key_row.addWidget(QLabel("Key:"))
        self.key_combo = QComboBox()
        self.key_combo.setMaxVisibleItems(20)
        self.key_combo.addItem("(None)")
        for name in KEY_MAP:
            self.key_combo.addItem(name)
        self.key_combo.setCurrentIndex(0)
        key_row.addWidget(self.key_combo)
        key_row.addStretch()
        kl.addLayout(key_row)

        desc_row = QHBoxLayout()
        desc_row.addWidget(QLabel("Description:"))
        self.desc_edit = QLineEdit()
        self.desc_edit.setMaxLength(31)
        self.desc_edit.setPlaceholderText("e.g. Ctrl+Z")
        desc_row.addWidget(self.desc_edit)
        kl.addLayout(desc_row)

        for cb in self.mod_checks.values():
            cb.stateChanged.connect(self._auto_desc)
        self.key_combo.currentTextChanged.connect(self._auto_desc)

        main_layout.addWidget(self.key_widget)

        # loop config panel
        self.loop_widget = QWidget()
        ll = QHBoxLayout(self.loop_widget)
        ll.setContentsMargins(0, 0, 0, 0)
        ll.addWidget(QLabel("Loop:"))
        self.loop_combo = QComboBox()
        self.loop_combo.addItems(["Loop 1", "Loop 2", "Loop 3"])
        ll.addWidget(self.loop_combo)
        ll.addStretch()
        main_layout.addWidget(self.loop_widget)
        self.loop_widget.setVisible(False)

    def _on_type_changed(self, idx):
        self.key_widget.setVisible(idx == 0)
        self.loop_widget.setVisible(idx == 1)

    def _auto_desc(self):
        kt = self.key_combo.currentText()
        if kt == "(None)":
            self.desc_edit.setText("")
            return
        parts = [n for n, cb in self.mod_checks.items() if cb.isChecked()]
        parts.append(kt)
        self.desc_edit.setText("+".join(parts))

    def get_config(self):
        if self.type_combo.currentIndex() == 0:
            mod = 0
            for name, bit in MODIFIER_BITS.items():
                if self.mod_checks[name].isChecked():
                    mod |= bit
            return {
                "type": 0, "mod": mod,
                "key": KEY_MAP.get(self.key_combo.currentText(), 0),
                "loop": 0, "desc": self.desc_edit.text(),
            }
        else:
            li = self.loop_combo.currentIndex()
            return {"type": 1, "mod": 0, "key": 0, "loop": li, "desc": f"Loop {li+1}"}

    def set_config(self, cfg):
        t = cfg.get("type", 0)
        self.type_combo.setCurrentIndex(t)

        if t == 0:
            mod = cfg.get("mod", 0)
            for name, bit in MODIFIER_BITS.items():
                self.mod_checks[name].setChecked(bool(mod & bit))
            kc = cfg.get("key", 0)
            if kc == 0:
                self.key_combo.setCurrentIndex(0)
            else:
                idx = self.key_combo.findText(KEYCODE_TO_NAME.get(kc, ""))
                if idx >= 0:
                    self.key_combo.setCurrentIndex(idx)
            desc = cfg.get("desc", "")
            if desc:
                self.desc_edit.setText(desc)
        else:
            li = cfg.get("loop", 0)
            if 0 <= li < NUM_LOOPS:
                self.loop_combo.setCurrentIndex(li)


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("FootPedal Configurator")
        self.setMinimumSize(600, 600)

        self.ble = BLEWorker()
        self.ble.scan_done.connect(self._on_scan_done)
        self.ble.connect_done.connect(self._on_connect_done)
        self.ble.disconnect_done.connect(self._on_disconnect_done)
        self.ble.command_done.connect(self._on_command_done)
        self.ble.error.connect(self._on_error)
        self.ble.start()

        self._pending = None
        self._write_queue = []
        self._devices = []

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # connection bar
        conn = QHBoxLayout()
        conn.addWidget(QLabel("Device:"))
        self.device_combo = QComboBox()
        self.device_combo.setMinimumWidth(200)
        conn.addWidget(self.device_combo)
        self.scan_btn = QPushButton("Scan")
        self.scan_btn.clicked.connect(self._on_scan)
        conn.addWidget(self.scan_btn)
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self._on_connect)
        conn.addWidget(self.connect_btn)
        conn.addStretch()
        root.addLayout(conn)

        # tabs
        self.tabs = QTabWidget()

        self.pedal_widgets = []
        for p_idx, p_name in enumerate(PROFILE_NAMES):
            page = QWidget()
            pl = QVBoxLayout(page)
            pedals = []
            for b_name in PEDAL_NAMES:
                pw = PedalWidget(b_name)
                pl.addWidget(pw)
                pedals.append(pw)
            pl.addStretch()
            self.pedal_widgets.append(pedals)
            self.tabs.addTab(page, p_name)

        # loops tab
        loops_page = QWidget()
        lo = QVBoxLayout(loops_page)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        content = QWidget()
        cl = QVBoxLayout(content)

        self.loop_editors = []
        for i in range(NUM_LOOPS):
            ed = LoopEditorWidget(f"Loop {i+1}")
            self.loop_editors.append(ed)
            cl.addWidget(ed)
        cl.addStretch()
        scroll.setWidget(content)
        lo.addWidget(scroll)
        self.tabs.addTab(loops_page, "Loops")

        root.addWidget(self.tabs)

        # action buttons
        btns = QHBoxLayout()
        btns.addStretch()
        self.read_btn = QPushButton("Read from Device")
        self.read_btn.clicked.connect(self._on_read)
        self.read_btn.setEnabled(False)
        btns.addWidget(self.read_btn)
        self.write_btn = QPushButton("Write to Device")
        self.write_btn.clicked.connect(self._on_write)
        self.write_btn.setEnabled(False)
        btns.addWidget(self.write_btn)
        root.addLayout(btns)

        self.status = QStatusBar()
        self.setStatusBar(self.status)
        self.status.showMessage("Not connected. Click Scan to find your FootPedal.")

    def closeEvent(self, event):
        self.ble.stop()
        event.accept()

    def _on_scan(self):
        self.scan_btn.setEnabled(False)
        self.status.showMessage("Scanning for BLE devices...")
        self.ble.enqueue("scan", timeout=5.0)

    def _on_connect(self):
        if self.ble.comm.is_connected:
            self.connect_btn.setEnabled(False)
            self.status.showMessage("Disconnecting...")
            self.ble.enqueue("disconnect")
            return
        idx = self.device_combo.currentIndex()
        if idx < 0 or idx >= len(self._devices):
            self.status.showMessage("No device selected.")
            return
        self.connect_btn.setEnabled(False)
        addr = self._devices[idx]["address"]
        self.status.showMessage(f"Connecting to {addr}...")
        self.ble.enqueue("connect", address=addr)

    def _on_read(self):
        self.read_btn.setEnabled(False)
        self.write_btn.setEnabled(False)
        self.status.showMessage("Reading profiles...")
        self._pending = "read_profiles"
        self.ble.enqueue("command", command={"cmd": "get_all"})

    def _on_write(self):
        self.read_btn.setEnabled(False)
        self.write_btn.setEnabled(False)
        self.status.showMessage("Writing to device...")
        self._pending = "write"
        self._write_queue = []
        for p in range(4):
            buttons = [self.pedal_widgets[p][b].get_config() for b in range(3)]
            self._write_queue.append({"cmd": "set", "profile": p, "buttons": buttons})
        for l in range(NUM_LOOPS):
            cfg = self.loop_editors[l].get_config()
            self._write_queue.append({
                "cmd": "set_loop", "loop": l,
                "repeat": cfg["repeat"], "steps": cfg["steps"],
            })
        self._write_next()

    def _write_next(self):
        if not self._write_queue:
            self.read_btn.setEnabled(True)
            self.write_btn.setEnabled(True)
            self.status.showMessage("All data written successfully!")
            self._pending = None
            return
        self.ble.enqueue("command", command=self._write_queue.pop(0))

    def _on_scan_done(self, devices):
        self.scan_btn.setEnabled(True)
        self._devices = devices
        self.device_combo.clear()
        for d in devices:
            self.device_combo.addItem(f"{d['name']} ({d['address']})")
        self.status.showMessage(
            f"Found {len(devices)} device(s)." if devices
            else "No FootPedal devices found. Is it powered on?"
        )

    def _on_connect_done(self, ok):
        self.connect_btn.setEnabled(True)
        if ok:
            self.connect_btn.setText("Disconnect")
            self.read_btn.setEnabled(True)
            self.write_btn.setEnabled(True)
            self.status.showMessage("Connected via BLE!")
        else:
            self.status.showMessage("Connection failed.")

    def _on_disconnect_done(self):
        self.connect_btn.setEnabled(True)
        self.connect_btn.setText("Connect")
        self.read_btn.setEnabled(False)
        self.write_btn.setEnabled(False)
        self.status.showMessage("Disconnected.")

    def _on_command_done(self, result):
        if "error" in result:
            self.status.showMessage(f"Error: {result['error']}")
            self.read_btn.setEnabled(True)
            self.write_btn.setEnabled(True)
            self._pending = None
            self._write_queue.clear()
            return

        if self._pending == "read_profiles":
            for prof in result.get("profiles", []):
                pi = prof.get("profile", 0)
                if 0 <= pi < 4:
                    for bi, btn in enumerate(prof.get("buttons", [])):
                        if bi < 3:
                            self.pedal_widgets[pi][bi].set_config(btn)
            self.status.showMessage("Profiles loaded. Reading loops...")
            self._pending = "read_loops"
            self.ble.enqueue("command", command={"cmd": "get_loops"})

        elif self._pending == "read_loops":
            for ld in result.get("loops", []):
                li = ld.get("loop", 0)
                if 0 <= li < NUM_LOOPS:
                    self.loop_editors[li].set_config(ld)
            self._pending = None
            self.read_btn.setEnabled(True)
            self.write_btn.setEnabled(True)
            self.status.showMessage("All data loaded from device.")

        elif self._pending == "write":
            if result.get("ok"):
                self._write_next()
            else:
                self.read_btn.setEnabled(True)
                self.write_btn.setEnabled(True)
                self.status.showMessage(f"Write error: {result}")
                self._pending = None
                self._write_queue.clear()

    def _on_error(self, msg):
        self.status.showMessage(f"BLE Error: {msg}")
        self.read_btn.setEnabled(self.ble.comm.is_connected)
        self.write_btn.setEnabled(self.ble.comm.is_connected)
        self.scan_btn.setEnabled(True)
        self.connect_btn.setEnabled(True)
        self._write_queue.clear()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setApplicationName("FootPedal Configurator")
    w = MainWindow()
    w.show()
    sys.exit(app.exec())
