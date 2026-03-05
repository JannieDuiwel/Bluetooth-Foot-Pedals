import asyncio
import json
from bleak import BleakClient, BleakScanner

CONFIG_SERVICE_UUID = "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
CONFIG_CMD_CHAR_UUID = "a1b2c3d4-e5f6-7890-abcd-ef1234567891"
CONFIG_RESPONSE_CHAR_UUID = "a1b2c3d4-e5f6-7890-abcd-ef1234567892"
DEVICE_NAME = "FootPedal"


class BLEComm:
    def __init__(self):
        self.client = None
        self.address = None
        self._response_event = asyncio.Event()
        self._response_data = ""

    def _on_notify(self, sender, data):
        self._response_data = data.decode("utf-8")
        self._response_event.set()

    async def scan(self, timeout=5.0):
        devices = await BleakScanner.discover(timeout=timeout)
        return [
            {"name": d.name, "address": d.address}
            for d in devices if d.name and DEVICE_NAME in d.name
        ]

    async def connect(self, address):
        try:
            self.client = BleakClient(address)
            await self.client.connect()
            self.address = address
            await self.client.start_notify(CONFIG_RESPONSE_CHAR_UUID, self._on_notify)
            return True
        except Exception as e:
            print(f"BLE connect error: {e}")
            self.client = None
            return False

    async def disconnect(self):
        if self.client and self.client.is_connected:
            try:
                await self.client.stop_notify(CONFIG_RESPONSE_CHAR_UUID)
            except Exception:
                pass
            await self.client.disconnect()
        self.client = None
        self.address = None

    @property
    def is_connected(self):
        return self.client is not None and self.client.is_connected

    async def send_command(self, command, timeout=5.0):
        if not self.is_connected:
            return {"error": "Not connected"}

        self._response_event.clear()
        self._response_data = ""
        await self.client.write_gatt_char(
            CONFIG_CMD_CHAR_UUID, json.dumps(command).encode("utf-8")
        )
        try:
            await asyncio.wait_for(self._response_event.wait(), timeout=timeout)
            return json.loads(self._response_data)
        except asyncio.TimeoutError:
            return {"error": "Timeout"}
        except json.JSONDecodeError:
            return {"error": f"Bad response: {self._response_data}"}

    async def ping(self):
        return await self.send_command({"cmd": "ping"})

    async def get_profile(self, profile):
        return await self.send_command({"cmd": "get", "profile": profile})

    async def get_all_profiles(self):
        return await self.send_command({"cmd": "get_all"})

    async def set_profile(self, profile, buttons):
        return await self.send_command({"cmd": "set", "profile": profile, "buttons": buttons})

    async def get_loop(self, loop_index):
        return await self.send_command({"cmd": "get_loop", "loop": loop_index})

    async def get_all_loops(self):
        return await self.send_command({"cmd": "get_loops"})

    async def set_loop(self, loop_index, repeat, steps):
        return await self.send_command({
            "cmd": "set_loop", "loop": loop_index,
            "repeat": repeat, "steps": steps,
        })
