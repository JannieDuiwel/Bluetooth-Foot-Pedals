#include <Arduino.h>
#include <BleKeyboard.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

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
    uint8_t type;       // 0=key, 1=loop
    uint8_t modifier;   // bit0=Ctrl, bit1=Shift, bit2=Alt, bit3=GUI
    uint8_t key;
    uint8_t loopIndex;  // which loop (0-2) when type=1
    char description[32];
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

BleKeyboard bleKeyboard("FootPedal", "FootPedal", 100);
Preferences preferences;

Profile profiles[NUM_PROFILES];
LoopConfig loops[NUM_LOOPS];
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
bool configClientConnected = false;
String pendingCommand = "";

const uint8_t profileColors[NUM_PROFILES][3] = {
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

void setLED(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LED_R_CH, r);
    ledcWrite(LED_G_CH, g);
    ledcWrite(LED_B_CH, b);
}

void setProfileLED(int profile) {
    if (profile >= 0 && profile < NUM_PROFILES)
        setLED(profileColors[profile][0], profileColors[profile][1], profileColors[profile][2]);
}

void ledOff() { setLED(0, 0, 0); }

void loadDefaults() {
    for (int p = 0; p < NUM_PROFILES; p++)
        for (int b = 0; b < NUM_BUTTONS; b++)
            memset(&profiles[p].buttons[b], 0, sizeof(ButtonConfig));
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
        i++;
    }
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
String profileToJson(int index) {
    JsonDocument doc;
    doc["profile"] = index;
    JsonArray arr = doc["buttons"].to<JsonArray>();
    for (int i = 0; i < NUM_BUTTONS; i++) {
        JsonObject btn = arr.add<JsonObject>();
        btn["type"] = profiles[index].buttons[i].type;
        btn["mod"] = profiles[index].buttons[i].modifier;
        btn["key"] = profiles[index].buttons[i].key;
        btn["loop"] = profiles[index].buttons[i].loopIndex;
        btn["desc"] = profiles[index].buttons[i].description;
    }
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
        for (int i = 0; i < NUM_BUTTONS; i++) {
            JsonObject btn = btns.add<JsonObject>();
            btn["type"] = profiles[p].buttons[i].type;
            btn["mod"] = profiles[p].buttons[i].modifier;
            btn["key"] = profiles[p].buttons[i].key;
            btn["loop"] = profiles[p].buttons[i].loopIndex;
            btn["desc"] = profiles[p].buttons[i].description;
        }
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
        response = "{\"pong\":true,\"version\":\"1.1\"}";
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
    else {
        response = "{\"error\":\"Unknown command\"}";
    }

    Serial.printf("CMD: %s -> %d bytes\n", cmd, response.length());
    pResponseCharacteristic->setValue(response.c_str());
    pResponseCharacteristic->notify();
}

class ConfigServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) override {
        configClientConnected = true;
        BLEDevice::startAdvertising();
    }
    void onDisconnect(BLEServer *pServer) override {
        configClientConnected = false;
        BLEDevice::startAdvertising();
    }
};

class CmdCharCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) override {
        String value = pCharacteristic->getValue().c_str();
        if (value.length() > 0) pendingCommand = value;
    }
};

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

    bleKeyboard.begin();
    BLEDevice::setMTU(517);

    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ConfigServerCallbacks());

    BLEService *pConfigService = pServer->createService(CONFIG_SERVICE_UUID);

    BLECharacteristic *pCmdChar = pConfigService->createCharacteristic(
        CONFIG_CMD_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    pCmdChar->setCallbacks(new CmdCharCallbacks());

    pResponseCharacteristic = pConfigService->createCharacteristic(
        CONFIG_RESPONSE_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    pResponseCharacteristic->addDescriptor(new BLE2902());

    pConfigService->start();

    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(CONFIG_SERVICE_UUID);
    pAdv->start();

    disconnectTime = millis();
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
            if (pressed && !pedalState[i] && (now - pedalDebounce[i] > DEBOUNCE_MS)) {
                pedalDebounce[i] = now;
                pedalState[i] = true;
                ButtonConfig &cfg = profiles[activeProfile].buttons[i];

                if (cfg.type == 0 && cfg.key != 0) {
                    pressKey(cfg.modifier, cfg.key);
                } else if (cfg.type == 1) {
                    if (activeLoop == cfg.loopIndex) stopLoop();
                    else { stopLoop(); startLoop(cfg.loopIndex); }
                }
            } else if (!pressed && pedalState[i]) {
                pedalState[i] = false;
            }
        }
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
