#include <Arduino.h>
#include <BleKeyboard.h>
#include <BleMouse.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>

#define VERSION "1.2.0"
#define GITHUB_REPO "JannieDuiwel/Bluetooth-Foot-Pedals"

// Pin config
#define PEDAL_1_PIN   32
#define PEDAL_2_PIN   33
#define PEDAL_3_PIN   25

#define ROTARY_POS_1  26
#define ROTARY_POS_2  27
#define ROTARY_POS_3  14
#define ROTARY_POS_4  12

#define LED_R_PIN     16
#define LED_G_PIN     17
#define LED_B_PIN     18
#define LED_R_CH      0
#define LED_G_CH      1
#define LED_B_CH      2

#define NUM_PROFILES    4
#define NUM_BUTTONS     3
#define NUM_LOOPS       3
#define MAX_LOOP_STEPS  20
#define DEBOUNCE_MS     20
#define FLASH_INTERVAL  500
#define FLASH_TIMEOUT   300000

#define CONFIG_SERVICE_UUID        "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
#define CONFIG_CMD_CHAR_UUID       "a1b2c3d4-e5f6-7890-abcd-ef1234567891"
#define CONFIG_RESPONSE_CHAR_UUID  "a1b2c3d4-e5f6-7890-abcd-ef1234567892"

struct ButtonConfig {
    uint8_t type;        // 0=key, 1=loop, 2=hold, 3=autoclicker
    uint8_t modifier;    // bit0=Ctrl, bit1=Shift, bit2=Alt, bit3=GUI
    uint8_t key;
    uint8_t loopIndex;   // which loop (0-2) when type=1
    char description[32];
    uint8_t click_hz;    // autoclicker: 1-100 Hz
    uint8_t click_button;// autoclicker: 0=left, 1=right, 2=middle
    uint8_t click_mode;  // autoclicker: 0=hold, 1=toggle
};

struct Profile {
    ButtonConfig buttons[NUM_BUTTONS];
};

struct LoopStep {
    uint8_t modifier;
    uint8_t key;
    uint16_t delay_ms;
};

struct LoopConfig {
    LoopStep steps[MAX_LOOP_STEPS];
    uint8_t numSteps;
    bool repeat;
};

// Forward declarations so the two subclasses can reference each other.
class FootPedalKeyboard;
class FootPedalMouse;
extern FootPedalKeyboard bleKeyboard;
extern FootPedalMouse bleMouse;

// Subclass BleMouse to restore keyboard as the server callback owner after
// the mouse's taskServer sets itself. Without this, mouse's callbacks override
// keyboard's and bleKeyboard.isConnected() never returns true.
class FootPedalMouse : public BleMouse {
public:
    FootPedalMouse() : BleMouse("FootPedal", "FootPedal", 100) {}
protected:
    void onStarted(BLEServer* pServer) override;  // defined after keyboard class
};

// Subclass BleKeyboard to hook onStarted() so we can add the config service
// to the SAME BLE server — avoids the two-server problem where onConnect()
// never fires for the keyboard.
class FootPedalKeyboard : public BleKeyboard {
public:
    FootPedalKeyboard() : BleKeyboard("FootPedal", "FootPedal", 100) {}
protected:
    void onStarted(BLEServer* pServer) override;
    void onConnect(BLEServer* pServer) override {
        BleKeyboard::onConnect(pServer);
        bleMouse.connectionStatus->connected = true;
        BLEDevice::startAdvertising();
    }
    void onDisconnect(BLEServer* pServer) override {
        BleKeyboard::onDisconnect(pServer);
        bleMouse.connectionStatus->connected = false;
    }
};
FootPedalKeyboard bleKeyboard;
FootPedalMouse bleMouse;

// After mouse finishes its taskServer setup (which sets pServer callbacks to
// mouse's connectionStatus), immediately restore keyboard as callback owner.
void FootPedalMouse::onStarted(BLEServer* pServer) {
    pServer->setCallbacks(&bleKeyboard);
}
Preferences preferences;

// Per-pedal autoclicker runtime state
unsigned long acLastClickMs[NUM_BUTTONS] = {0, 0, 0};
bool acToggleActive[NUM_BUTTONS] = {false, false, false};

Profile profiles[NUM_PROFILES];
LoopConfig loops[NUM_LOOPS];
uint8_t ledScale[3] = {200, 200, 200};  // per-channel PWM scale (R, G, B)
int activeProfile = 0;

int activeLoop = -1;
int loopStepIndex = 0;
unsigned long loopNextStepTime = 0;

const int pedalPins[NUM_BUTTONS] = { PEDAL_1_PIN, PEDAL_2_PIN, PEDAL_3_PIN };
bool pedalState[NUM_BUTTONS] = { false, false, false };
unsigned long pedalDebounce[NUM_BUTTONS] = { 0, 0, 0 };

const int rotaryPins[NUM_PROFILES] = { ROTARY_POS_1, ROTARY_POS_2, ROTARY_POS_3, ROTARY_POS_4 };
int lastRotaryPos = -1;

bool bleConnected = false;
bool wasBleConnected = false;
unsigned long disconnectTime = 0;
unsigned long lastFlashToggle = 0;
bool flashState = false;
bool flashTimedOut = false;

BLECharacteristic *pResponseCharacteristic = nullptr;
String pendingCommand = "";

uint8_t profileColors[NUM_PROFILES][3] = {
    {255, 0,   0  },   // red
    {0,   255, 0  },   // green
    {0,   0,   255},   // blue
    {255, 0,   255},   // purple
};

void pressKey(uint8_t modifier, uint8_t key) {
    if (modifier & 0x01) bleKeyboard.press(KEY_LEFT_CTRL);
    if (modifier & 0x02) bleKeyboard.press(KEY_LEFT_SHIFT);
    if (modifier & 0x04) bleKeyboard.press(KEY_LEFT_ALT);
    if (modifier & 0x08) bleKeyboard.press(KEY_LEFT_GUI);
    bleKeyboard.press(key);
    delay(10);
    bleKeyboard.releaseAll();
}

void holdKey(uint8_t modifier, uint8_t key) {
    if (modifier & 0x01) bleKeyboard.press(KEY_LEFT_CTRL);
    if (modifier & 0x02) bleKeyboard.press(KEY_LEFT_SHIFT);
    if (modifier & 0x04) bleKeyboard.press(KEY_LEFT_ALT);
    if (modifier & 0x08) bleKeyboard.press(KEY_LEFT_GUI);
    bleKeyboard.press(key);
}

void releaseKey(uint8_t modifier, uint8_t key) {
    bleKeyboard.release(key);
    if (modifier & 0x01) bleKeyboard.release(KEY_LEFT_CTRL);
    if (modifier & 0x02) bleKeyboard.release(KEY_LEFT_SHIFT);
    if (modifier & 0x04) bleKeyboard.release(KEY_LEFT_ALT);
    if (modifier & 0x08) bleKeyboard.release(KEY_LEFT_GUI);
}

void setLED(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LED_R_CH, (uint16_t)r * ledScale[0] / 255);
    ledcWrite(LED_G_CH, (uint16_t)g * ledScale[1] / 255);
    ledcWrite(LED_B_CH, (uint16_t)b * ledScale[2] / 255);
}

void setProfileLED(int profile) {
    if (profile >= 0 && profile < NUM_PROFILES)
        setLED(profileColors[profile][0], profileColors[profile][1], profileColors[profile][2]);
}

void ledOff() { setLED(0, 0, 0); }

void loadDefaults() {
    for (int p = 0; p < NUM_PROFILES; p++) {
        for (int b = 0; b < NUM_BUTTONS; b++) {
            memset(&profiles[p].buttons[b], 0, sizeof(ButtonConfig));
            profiles[p].buttons[b].click_hz = 5;
            profiles[p].buttons[b].click_button = 0;
            profiles[p].buttons[b].click_mode = 0;
        }
    }
    for (int l = 0; l < NUM_LOOPS; l++) {
        loops[l].numSteps = 0;
        loops[l].repeat = true;
    }
}

void saveProfile(int index) {
    JsonDocument doc;
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (int i = 0; i < NUM_BUTTONS; i++) {
        JsonObject btn = arr.add<JsonObject>();
        btn["type"] = profiles[index].buttons[i].type;
        btn["mod"] = profiles[index].buttons[i].modifier;
        btn["key"] = profiles[index].buttons[i].key;
        btn["loop"] = profiles[index].buttons[i].loopIndex;
        btn["desc"] = profiles[index].buttons[i].description;
        btn["chz"] = profiles[index].buttons[i].click_hz;
        btn["cbtn"] = profiles[index].buttons[i].click_button;
        btn["cmode"] = profiles[index].buttons[i].click_mode;
    }
    String json;
    serializeJson(doc, json);
    char key[12];
    snprintf(key, sizeof(key), "profile_%d", index);
    preferences.putString(key, json);
}

void loadProfile(int index) {
    char key[12];
    snprintf(key, sizeof(key), "profile_%d", index);
    String json = preferences.getString(key, "");
    if (json.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return;

    JsonArray arr = doc["buttons"].as<JsonArray>();
    int i = 0;
    for (JsonObject btn : arr) {
        if (i >= NUM_BUTTONS) break;
        profiles[index].buttons[i].type = btn["type"] | 0;
        profiles[index].buttons[i].modifier = btn["mod"] | 0;
        profiles[index].buttons[i].key = btn["key"] | 0;
        profiles[index].buttons[i].loopIndex = btn["loop"] | 0;
        strlcpy(profiles[index].buttons[i].description, btn["desc"] | "",
                sizeof(profiles[index].buttons[i].description));
        profiles[index].buttons[i].click_hz = constrain((int)(btn["chz"] | 5), 1, 100);
        profiles[index].buttons[i].click_button = constrain((int)(btn["cbtn"] | 0), 0, 2);
        profiles[index].buttons[i].click_mode = constrain((int)(btn["cmode"] | 0), 0, 1);
        i++;
    }
}

void saveLedScale() {
    preferences.putBytes("led_scale", ledScale, 3);
}

void loadLedScale() {
    preferences.getBytes("led_scale", ledScale, 3);
}

void saveProfileColors() {
    preferences.putBytes("prof_colors", profileColors, sizeof(profileColors));
}

void loadProfileColors() {
    preferences.getBytes("prof_colors", profileColors, sizeof(profileColors));
}

void saveLoop(int index) {
    JsonDocument doc;
    doc["repeat"] = loops[index].repeat;
    JsonArray arr = doc["steps"].to<JsonArray>();
    for (int i = 0; i < loops[index].numSteps; i++) {
        JsonObject step = arr.add<JsonObject>();
        step["mod"] = loops[index].steps[i].modifier;
        step["key"] = loops[index].steps[i].key;
        step["delay"] = loops[index].steps[i].delay_ms;
    }
    String json;
    serializeJson(doc, json);
    char key[8];
    snprintf(key, sizeof(key), "loop_%d", index);
    preferences.putString(key, json);
}

void loadLoop(int index) {
    char key[8];
    snprintf(key, sizeof(key), "loop_%d", index);
    String json = preferences.getString(key, "");
    if (json.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return;

    loops[index].repeat = doc["repeat"] | true;
    JsonArray arr = doc["steps"].as<JsonArray>();
    int i = 0;
    for (JsonObject step : arr) {
        if (i >= MAX_LOOP_STEPS) break;
        loops[index].steps[i].modifier = step["mod"] | 0;
        loops[index].steps[i].key = step["key"] | 0;
        loops[index].steps[i].delay_ms = step["delay"] | 500;
        i++;
    }
    loops[index].numSteps = i;
}

void loadAllData() {
    loadDefaults();
    preferences.begin("footpedal", true);
    for (int i = 0; i < NUM_PROFILES; i++) loadProfile(i);
    for (int i = 0; i < NUM_LOOPS; i++) loadLoop(i);
    loadLedScale();
    loadProfileColors();
    preferences.end();
}

void startLoop(int loopIdx) {
    if (loopIdx < 0 || loopIdx >= NUM_LOOPS || loops[loopIdx].numSteps == 0) return;
    activeLoop = loopIdx;
    loopStepIndex = 0;
    loopNextStepTime = millis();
}

void stopLoop() {
    activeLoop = -1;
    loopStepIndex = 0;
}

void tickLoop() {
    if (activeLoop < 0) return;
    LoopConfig &lc = loops[activeLoop];
    if (lc.numSteps == 0) { stopLoop(); return; }

    unsigned long now = millis();
    if (now < loopNextStepTime) return;

    LoopStep &step = lc.steps[loopStepIndex];
    pressKey(step.modifier, step.key);
    loopStepIndex++;

    if (loopStepIndex >= lc.numSteps) {
        if (lc.repeat) loopStepIndex = 0;
        else { stopLoop(); return; }
    }
    loopNextStepTime = now + step.delay_ms;
}

// JSON serialization for BLE config responses
static void serializeButton(JsonObject btn, const ButtonConfig &cfg) {
    btn["type"] = cfg.type;
    btn["mod"] = cfg.modifier;
    btn["key"] = cfg.key;
    btn["loop"] = cfg.loopIndex;
    btn["desc"] = cfg.description;
    btn["chz"] = cfg.click_hz;
    btn["cbtn"] = cfg.click_button;
    btn["cmode"] = cfg.click_mode;
}

String profileToJson(int index) {
    JsonDocument doc;
    doc["profile"] = index;
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (int i = 0; i < NUM_BUTTONS; i++)
        serializeButton(arr.add<JsonObject>(), profiles[index].buttons[i]);
    String json;
    serializeJson(doc, json);
    return json;
}

String allProfilesToJson() {
    JsonDocument doc;
    JsonArray arr = doc["profiles"].to<JsonArray>();
    for (int p = 0; p < NUM_PROFILES; p++) {
        JsonObject prof = arr.add<JsonObject>();
        prof["profile"] = p;
        JsonArray btns = prof["buttons"].to<JsonArray>();
        for (int i = 0; i < NUM_BUTTONS; i++)
            serializeButton(btns.add<JsonObject>(), profiles[p].buttons[i]);
    }
    String json;
    serializeJson(doc, json);
    return json;
}

String loopToJson(int index) {
    JsonDocument doc;
    doc["loop"] = index;
    doc["repeat"] = loops[index].repeat;
    JsonArray arr = doc["steps"].to<JsonArray>();
    for (int i = 0; i < loops[index].numSteps; i++) {
        JsonObject step = arr.add<JsonObject>();
        step["mod"] = loops[index].steps[i].modifier;
        step["key"] = loops[index].steps[i].key;
        step["delay"] = loops[index].steps[i].delay_ms;
    }
    String json;
    serializeJson(doc, json);
    return json;
}

String allLoopsToJson() {
    JsonDocument doc;
    JsonArray arr = doc["loops"].to<JsonArray>();
    for (int l = 0; l < NUM_LOOPS; l++) {
        JsonObject loop = arr.add<JsonObject>();
        loop["loop"] = l;
        loop["repeat"] = loops[l].repeat;
        JsonArray steps = loop["steps"].to<JsonArray>();
        for (int i = 0; i < loops[l].numSteps; i++) {
            JsonObject step = steps.add<JsonObject>();
            step["mod"] = loops[l].steps[i].modifier;
            step["key"] = loops[l].steps[i].key;
            step["delay"] = loops[l].steps[i].delay_ms;
        }
    }
    String json;
    serializeJson(doc, json);
    return json;
}

void notifyOta(const char* status, const char* msg = nullptr) {
    if (!pResponseCharacteristic) return;
    char buf[128];
    if (msg)
        snprintf(buf, sizeof(buf), "{\"ota\":\"%s\",\"msg\":\"%s\"}", status, msg);
    else
        snprintf(buf, sizeof(buf), "{\"ota\":\"%s\"}", status);
    pResponseCharacteristic->setValue(buf);
    pResponseCharacteristic->notify();
}

void doOtaCheck() {
    char ssid[64] = {0};
    char pass[64] = {0};
    preferences.begin("footpedal", true);
    preferences.getString("wifi_ssid", ssid, sizeof(ssid));
    preferences.getString("wifi_pass", pass, sizeof(pass));
    preferences.end();

    if (strlen(ssid) == 0) {
        notifyOta("error", "No WiFi credentials");
        return;
    }

    notifyOta("checking");
    WiFi.begin(ssid, pass);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(200);

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        notifyOta("error", "WiFi connect failed");
        return;
    }

    HTTPClient http;
    http.begin("https://api.github.com/repos/" GITHUB_REPO "/releases/latest");
    http.addHeader("User-Agent", "FootPedal/" VERSION);
    int code = http.GET();
    if (code != 200) {
        http.end();
        WiFi.disconnect(true);
        notifyOta("error", "GitHub API failed");
        return;
    }

    JsonDocument releaseDoc;
    if (deserializeJson(releaseDoc, http.getStream())) {
        http.end();
        WiFi.disconnect(true);
        notifyOta("error", "JSON parse failed");
        return;
    }
    http.end();

    const char* tag = releaseDoc["tag_name"];
    if (!tag) { WiFi.disconnect(true); notifyOta("error", "No tag"); return; }

    // Strip leading 'v' for comparison
    const char* remoteVer = (tag[0] == 'v') ? tag + 1 : tag;
    if (strcmp(remoteVer, VERSION) <= 0) {
        WiFi.disconnect(true);
        notifyOta("up_to_date");
        return;
    }

    // Find firmware.bin asset
    String binUrl;
    for (JsonObject asset : releaseDoc["assets"].as<JsonArray>()) {
        const char* name = asset["name"];
        if (name && strcmp(name, "firmware.bin") == 0) {
            binUrl = asset["browser_download_url"].as<String>();
            break;
        }
    }
    if (binUrl.isEmpty()) {
        WiFi.disconnect(true);
        notifyOta("error", "No firmware.bin asset");
        return;
    }

    notifyOta("updating");
    HTTPClient dlHttp;
    dlHttp.begin(binUrl);
    dlHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int dlCode = dlHttp.GET();
    if (dlCode != 200) {
        dlHttp.end();
        WiFi.disconnect(true);
        notifyOta("error", "Download failed");
        return;
    }

    int contentLen = dlHttp.getSize();
    if (!Update.begin(contentLen > 0 ? contentLen : UPDATE_SIZE_UNKNOWN)) {
        dlHttp.end();
        WiFi.disconnect(true);
        notifyOta("error", "Update.begin failed");
        return;
    }

    size_t written = Update.writeStream(*dlHttp.getStreamPtr());
    dlHttp.end();
    WiFi.disconnect(true);

    if (written != (size_t)contentLen || !Update.end(true)) {
        notifyOta("error", "Flash write failed");
        return;
    }

    notifyOta("done");
    delay(500);
    ESP.restart();
}

void handleConfigCommand(const String &cmdStr) {
    JsonDocument doc;
    if (deserializeJson(doc, cmdStr)) {
        pResponseCharacteristic->setValue("{\"error\":\"Invalid JSON\"}");
        pResponseCharacteristic->notify();
        return;
    }

    const char* cmd = doc["cmd"];
    String response;

    if (!cmd) {
        pResponseCharacteristic->setValue("{\"error\":\"Missing cmd\"}");
        pResponseCharacteristic->notify();
        return;
    }

    if (strcmp(cmd, "ping") == 0) {
        response = "{\"pong\":true,\"version\":\"" VERSION "\"}";
    }
    else if (strcmp(cmd, "get") == 0) {
        int p = doc["profile"] | 0;
        response = (p >= 0 && p < NUM_PROFILES) ? profileToJson(p) : "{\"error\":\"Invalid profile\"}";
    }
    else if (strcmp(cmd, "get_all") == 0) {
        response = allProfilesToJson();
    }
    else if (strcmp(cmd, "set") == 0) {
        int p = doc["profile"] | -1;
        if (p < 0 || p >= NUM_PROFILES) {
            response = "{\"error\":\"Invalid profile\"}";
        } else {
            JsonArray btns = doc["buttons"].as<JsonArray>();
            int i = 0;
            for (JsonObject btn : btns) {
                if (i >= NUM_BUTTONS) break;
                profiles[p].buttons[i].type = btn["type"] | 0;
                profiles[p].buttons[i].modifier = btn["mod"] | 0;
                profiles[p].buttons[i].key = btn["key"] | 0;
                profiles[p].buttons[i].loopIndex = btn["loop"] | 0;
                strlcpy(profiles[p].buttons[i].description, btn["desc"] | "",
                        sizeof(profiles[p].buttons[i].description));
                profiles[p].buttons[i].click_hz = constrain((int)(btn["chz"] | 5), 1, 100);
                profiles[p].buttons[i].click_button = constrain((int)(btn["cbtn"] | 0), 0, 2);
                profiles[p].buttons[i].click_mode = constrain((int)(btn["cmode"] | 0), 0, 1);
                i++;
            }
            preferences.begin("footpedal", false);
            saveProfile(p);
            preferences.end();
            response = "{\"ok\":true}";
        }
    }
    else if (strcmp(cmd, "get_loop") == 0) {
        int l = doc["loop"] | -1;
        response = (l >= 0 && l < NUM_LOOPS) ? loopToJson(l) : "{\"error\":\"Invalid loop\"}";
    }
    else if (strcmp(cmd, "get_loops") == 0) {
        response = allLoopsToJson();
    }
    else if (strcmp(cmd, "set_loop") == 0) {
        int l = doc["loop"] | -1;
        if (l < 0 || l >= NUM_LOOPS) {
            response = "{\"error\":\"Invalid loop\"}";
        } else {
            loops[l].repeat = doc["repeat"] | true;
            JsonArray steps = doc["steps"].as<JsonArray>();
            int i = 0;
            for (JsonObject step : steps) {
                if (i >= MAX_LOOP_STEPS) break;
                loops[l].steps[i].modifier = step["mod"] | 0;
                loops[l].steps[i].key = step["key"] | 0;
                loops[l].steps[i].delay_ms = step["delay"] | 500;
                i++;
            }
            loops[l].numSteps = i;
            if (activeLoop == l) stopLoop();
            preferences.begin("footpedal", false);
            saveLoop(l);
            preferences.end();
            response = "{\"ok\":true}";
        }
    }
    else if (strcmp(cmd, "get_led") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"led_scale\":true,\"r\":%d,\"g\":%d,\"b\":%d}",
                 ledScale[0], ledScale[1], ledScale[2]);
        response = buf;
    }
    else if (strcmp(cmd, "set_led") == 0) {
        ledScale[0] = constrain((int)(doc["r"] | 200), 0, 255);
        ledScale[1] = constrain((int)(doc["g"] | 200), 0, 255);
        ledScale[2] = constrain((int)(doc["b"] | 200), 0, 255);
        preferences.begin("footpedal", false);
        saveLedScale();
        preferences.end();
        setProfileLED(activeProfile);
        response = "{\"ok\":true}";
    }
    else if (strcmp(cmd, "get_colors") == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"colors\":[[%d,%d,%d],[%d,%d,%d],[%d,%d,%d],[%d,%d,%d]]}",
            profileColors[0][0], profileColors[0][1], profileColors[0][2],
            profileColors[1][0], profileColors[1][1], profileColors[1][2],
            profileColors[2][0], profileColors[2][1], profileColors[2][2],
            profileColors[3][0], profileColors[3][1], profileColors[3][2]);
        response = buf;
    }
    else if (strcmp(cmd, "set_color") == 0) {
        int p = doc["profile"] | -1;
        if (p < 0 || p >= NUM_PROFILES) {
            response = "{\"error\":\"Invalid profile\"}";
        } else {
            profileColors[p][0] = constrain((int)(doc["r"] | 0), 0, 255);
            profileColors[p][1] = constrain((int)(doc["g"] | 0), 0, 255);
            profileColors[p][2] = constrain((int)(doc["b"] | 0), 0, 255);
            preferences.begin("footpedal", false);
            saveProfileColors();
            preferences.end();
            setProfileLED(activeProfile);
            response = "{\"ok\":true}";
        }
    }
    else if (strcmp(cmd, "set_wifi") == 0) {
        const char* ssid = doc["ssid"] | "";
        const char* pass = doc["pass"] | "";
        if (strlen(ssid) == 0) {
            response = "{\"error\":\"Missing ssid\"}";
        } else {
            preferences.begin("footpedal", false);
            preferences.putString("wifi_ssid", ssid);
            preferences.putString("wifi_pass", pass);
            preferences.end();
            response = "{\"ok\":true}";
        }
    }
    else if (strcmp(cmd, "ota_check") == 0) {
        // Run OTA in a background task so this function returns and the BLE
        // response is flushed before we block on WiFi/HTTP.
        response = "{\"ok\":true}";
        pResponseCharacteristic->setValue(response.c_str());
        pResponseCharacteristic->notify();
        doOtaCheck();
        return;
    }
    else {
        response = "{\"error\":\"Unknown command\"}";
    }

    Serial.printf("CMD: %s -> %d bytes\n", cmd, response.length());
    pResponseCharacteristic->setValue(response.c_str());
    pResponseCharacteristic->notify();
}

class CmdCharCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) override {
        String value = pCharacteristic->getValue().c_str();
        if (value.length() > 0) pendingCommand = value;
    }
};

// Called by BleKeyboard::begin() after HID services are set up, before advertising.
// We add our config service here so it shares the same BLE server and the
// keyboard onConnect/onDisconnect callbacks fire correctly.
void FootPedalKeyboard::onStarted(BLEServer* pServer) {
    BLEService* pConfigService = pServer->createService(CONFIG_SERVICE_UUID);

    BLECharacteristic* pCmdChar = pConfigService->createCharacteristic(
        CONFIG_CMD_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCmdChar->setCallbacks(new CmdCharCallbacks());

    pResponseCharacteristic = pConfigService->createCharacteristic(
        CONFIG_RESPONSE_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pResponseCharacteristic->addDescriptor(new BLE2902());

    pConfigService->start();
    Serial.println("Config service added to keyboard server.");
}

int readRotarySwitch() {
    for (int i = 0; i < NUM_PROFILES; i++)
        if (digitalRead(rotaryPins[i]) == LOW) return i;
    return 0;
}

void setup() {
    Serial.begin(115200);

    for (int i = 0; i < NUM_BUTTONS; i++) pinMode(pedalPins[i], INPUT_PULLUP);
    for (int i = 0; i < NUM_PROFILES; i++) pinMode(rotaryPins[i], INPUT_PULLUP);

    ledcSetup(LED_R_CH, 5000, 8);
    ledcSetup(LED_G_CH, 5000, 8);
    ledcSetup(LED_B_CH, 5000, 8);
    ledcAttachPin(LED_R_PIN, LED_R_CH);
    ledcAttachPin(LED_G_PIN, LED_G_CH);
    ledcAttachPin(LED_B_PIN, LED_B_CH);
    ledOff();

    loadAllData();
    activeProfile = readRotarySwitch();
    lastRotaryPos = activeProfile;

    // Mouse begins first; FootPedalMouse::onStarted() will restore keyboard as
    // the server callback owner after mouse's taskServer sets its own callbacks.
    Serial.println("Starting BLE...");
    bleMouse.begin();
    bleKeyboard.begin();

    // Override the library's SC+MITM+Bond security to plain Bond — much more
    // compatible with Windows 11 without requiring passkey confirmation.
    BLESecurity* pSec = new BLESecurity();
    pSec->setAuthenticationMode(ESP_LE_AUTH_BOND);

    BLEDevice::setMTU(517);
    Serial.println("BLE started. Setup complete.");

    disconnectTime = millis();

    // Check for OTA update on boot if WiFi credentials are stored
    {
        char ssid[64] = {0};
        preferences.begin("footpedal", true);
        preferences.getString("wifi_ssid", ssid, sizeof(ssid));
        preferences.end();
        if (strlen(ssid) > 0) {
            Serial.println("WiFi credentials found, checking for OTA update...");
            doOtaCheck();
        }
    }
}

void loop() {
    unsigned long now = millis();
    bleConnected = bleKeyboard.isConnected();

    // LED: profile colour when connected, flash red when not (5 min timeout)
    if (bleConnected) {
        flashTimedOut = false;
        wasBleConnected = true;
        setProfileLED(activeProfile);
    } else {
        if (wasBleConnected) {
            wasBleConnected = false;
            disconnectTime = now;
        }
        if (!flashTimedOut) {
            if (now - disconnectTime > FLASH_TIMEOUT) { flashTimedOut = true; ledOff(); }
            else if (now - lastFlashToggle > FLASH_INTERVAL) {
                lastFlashToggle = now;
                flashState = !flashState;
                flashState ? setLED(255, 0, 0) : ledOff();
            }
        }
    }

    // rotary switch
    int curProfile = readRotarySwitch();
    if (curProfile != lastRotaryPos) {
        lastRotaryPos = curProfile;
        activeProfile = curProfile;
        stopLoop();
        if (bleConnected) setProfileLED(activeProfile);
    }

    // pedals
    if (bleConnected) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            bool pressed = (digitalRead(pedalPins[i]) == LOW);
            ButtonConfig &cfg = profiles[activeProfile].buttons[i];

            if (pressed && !pedalState[i] && (now - pedalDebounce[i] > DEBOUNCE_MS)) {
                pedalDebounce[i] = now;
                pedalState[i] = true;

                if (cfg.type == 0 && cfg.key != 0) {
                    pressKey(cfg.modifier, cfg.key);
                } else if (cfg.type == 1) {
                    if (activeLoop == cfg.loopIndex) stopLoop();
                    else { stopLoop(); startLoop(cfg.loopIndex); }
                } else if (cfg.type == 2 && cfg.key != 0) {
                    holdKey(cfg.modifier, cfg.key);
                } else if (cfg.type == 3) {
                    if (cfg.click_mode == 1) {
                        // toggle mode: flip on press-down
                        acToggleActive[i] = !acToggleActive[i];
                        acLastClickMs[i] = now;
                    }
                    // hold mode: clicking starts via the acLastClickMs check below
                }
            } else if (!pressed && pedalState[i]) {
                pedalState[i] = false;
                if (cfg.type == 2 && cfg.key != 0) {
                    releaseKey(cfg.modifier, cfg.key);
                } else if (cfg.type == 3 && cfg.click_mode == 0) {
                    // hold mode: stop clicking on release
                    acToggleActive[i] = false;
                }
            }

            // Autoclicker ticking
            if (cfg.type == 3) {
                bool shouldClick = false;
                if (cfg.click_mode == 0) {
                    // hold mode: click while pedal is held
                    shouldClick = pedalState[i];
                } else {
                    // toggle mode: click while toggled on
                    shouldClick = acToggleActive[i];
                }

                if (shouldClick) {
                    unsigned long intervalMs = 1000UL / constrain((int)cfg.click_hz, 1, 100);
                    if (now - acLastClickMs[i] >= intervalMs) {
                        acLastClickMs[i] = now;
                        if (cfg.click_button == 1) bleMouse.click(MOUSE_RIGHT);
                            else if (cfg.click_button == 2) bleMouse.click(MOUSE_MIDDLE);
                            else bleMouse.click(MOUSE_LEFT);
                    }
                }
            }
        }
    } else {
        // BLE disconnected: cancel any active autoclicker toggles
        for (int i = 0; i < NUM_BUTTONS; i++) acToggleActive[i] = false;
    }

    // run active loop
    if (bleConnected) tickLoop();
    else if (activeLoop >= 0) stopLoop();

    // handle BLE config commands
    if (pendingCommand.length() > 0) {
        handleConfigCommand(pendingCommand);
        pendingCommand = "";
    }

    delay(5);
}
