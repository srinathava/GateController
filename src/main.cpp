#include <cstdio>
#include <sstream>
#include <Servo.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <MQTT.h>
#include <EEPROM.h>

#include "credentials.hpp"
#include "ota_update.hpp"

WiFiClient net;
MQTTClient client;

const String GATE_ID = "7";

const int LIMIT_SWITCH_PIN = 5;
const int SERVO_PIN = 2;

// Changing this string will invalidate the existing contents.
const std::string CHKSUM_STR = "eeprom_init_v3";

int maxPos = 125; // close (increase to move further left)
int minPos = 25; // open (decrease to move further right)
int midPos = 80; // middle
int pos = midPos;
bool calibrated = false;

Servo myservo;

std::string gateCmd = "";

std::string gatePos = "middle";

void connect() {
    Serial.print("checking wifi...");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(1000);
    }
    Serial.println("\nconnected! Local IP: ");
    Serial.print(WiFi.localIP());

    Serial.print("\nconnecting...");
    String clientId = "savadhan-nodemcu-gate-" + GATE_ID;
    while (!client.connect(clientId.c_str())) {
        Serial.print(".");
        delay(1000);
    }

    Serial.println("\nconnected!");

    client.publish("/heartbeat/" + GATE_ID, "hello");
    client.subscribe("/gatecmd/" + GATE_ID);
}

void messageReceived(String &topic, String &payload) {
    Serial.println("incoming: " + topic + " - " + payload);
    gateCmd = payload.c_str();
}

void printState(int pos, int buttonState) {
    char buf[128];
    snprintf(buf, 128, "buttonState = %d, pos = %d\n", buttonState, pos);
    Serial.print(buf);
}

void moveServoTo(int finalPos, bool check, int delayT) {
    Serial.print("Moving servo to ");
    Serial.print(finalPos);
    Serial.print(" with check ");
    Serial.println(check);

    using CmpFcn = std::function<bool(int, int)>;

    CmpFcn leFcn = std::less_equal<int>{};
    CmpFcn geFcn = std::greater_equal<int>{};

    int dpos = (finalPos > pos) ? 2 : -2;
    auto cmpFcn = (finalPos > pos) ? leFcn : geFcn;
    if (pos == finalPos) {
        return;
    }

    int buttonState = 1;
    for (pos += dpos; cmpFcn(pos, finalPos); pos += dpos) {
        myservo.write(pos);
        delay(delayT);
        buttonState = digitalRead(LIMIT_SWITCH_PIN);
        printState(pos, buttonState);
        if (check && !buttonState) {
            break;
        }
    }

    if (check && !buttonState) {
        pos -= dpos;
        myservo.write(pos);
        delay(delayT);
        buttonState = digitalRead(LIMIT_SWITCH_PIN);
        printState(pos, buttonState);
    }
}

void writeChecksum(int* addr) {
    for (size_t i=0; i < CHKSUM_STR.length(); ++i) {
        EEPROM.write(*addr, CHKSUM_STR[i]);
        (*addr)++;
    }
}

std::string readChecksum(int* addr) {
    std::string result;
    for (size_t i=0; i < CHKSUM_STR.length(); ++i) {
        result.push_back(EEPROM.read(*addr));
        (*addr)++;
    }
    return result;
}

void calibrate() {
    Serial.println("Calibrating");
    moveServoTo(150, true, 60);
    maxPos = pos;
    
    moveServoTo(midPos, false, 60);

    moveServoTo(10, true, 60);
    minPos = pos;

    midPos = (maxPos + minPos)/2;
    moveServoTo(midPos, false, 15);

    gatePos = "middle";

    EEPROM.begin(512);
    int addr = 0;
    writeChecksum(&addr);
    EEPROM.put(addr, minPos); addr += sizeof(minPos);
    EEPROM.put(addr, maxPos); addr += sizeof(maxPos);
    EEPROM.commit();

    calibrated = true;
}

void setup() {
    Serial.begin(115200);
    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
    myservo.attach(SERVO_PIN);
    
    myservo.write(midPos);

    gatePos = "middle";

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    client.begin(MQTT_CONTROLLER_IP, net);
    client.onMessage(messageReceived);

    EEPROM.begin(512);
    int addr = 0;
    auto chksum = readChecksum(&addr);
    if (chksum == CHKSUM_STR) {
        Serial.print("Reading positions from EEROM: minPos = ");
        EEPROM.get(addr, minPos); addr += sizeof(minPos);
        Serial.print(minPos);

        EEPROM.get(addr, maxPos); addr += sizeof(maxPos);
        Serial.print(", maxPos = ");
        Serial.println(maxPos);

        calibrated = true;
    } else {
        Serial.print("Uninitialized data in EEPROM: ");
        Serial.println(chksum.c_str());
    }

    connect();
}

unsigned long lastMillis = 0;

void openClose(const std::string& finalPos) {
    if (!calibrated) {
        Serial.println("Calibrate first!");
        return;
    }

    Serial.print("Processing move cmd: ");
    Serial.println(finalPos.c_str());

    int pos1, pos2;
    if (finalPos == "open") {
        pos1 = minPos + 12;
        pos2 = minPos;
    } else {
        pos1 = maxPos - 12;
        pos2 = maxPos;
    }
    
    if (gatePos != finalPos) {
        moveServoTo(pos1, false, 15);
        moveServoTo(pos2, true, 60);
    }
    gatePos = finalPos;
}

void loop() {
    if (!client.connected()) {   
        connect();    
    } else {
        client.loop();
        delay(10);  // <- fixes some issues with WiFi stability
    }
    
    handleOTAUpdate(GATE_ID);

    if (gateCmd == "open" || gateCmd == "close") {
        openClose(gateCmd);
    } else if (gateCmd == "middle") {
        Serial.println("Processing middle cmd");
        if (gatePos != "middle") {
            moveServoTo(midPos, false, 15);
        }
        gatePos = "middle";
    } else if (gateCmd == "calibrate") {
        calibrate();
    } else if (gateCmd != "") {
        Serial.print("Invalid command: ");
        Serial.println(gateCmd.c_str());
        gateCmd = "";
    }

    if (gateCmd != "") {
        client.publish("/gateack/" + GATE_ID, gateCmd.c_str());
        gateCmd = "";
    }

    auto millisNow = millis();
    if (millisNow - lastMillis > 3000) {
        std::ostringstream os;
        os << "{"
            << "\"gatePos\" : \"" << gatePos << "\", "
            << "\"ipAddress\": \"" << WiFi.localIP().toString().c_str() << "\""
            << "}";
        auto status = os.str();
        client.publish("/heartbeat/" + GATE_ID, status.c_str());
        lastMillis = millisNow;
    }
}