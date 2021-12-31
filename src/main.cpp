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

std::string GATE_ID = "4";

const int LIMIT_SWITCH_PIN = D1;
const int SERVO_PWM_PIN = D4;
const int SERVO_ENABLE_PIN = D8;

// Changing this string will invalidate the existing contents.
const std::string CHKSUM_STR = "eeprom_init_v9";

int maxPos = 125; // close (increase to move further left)
int minPos = 25; // open (decrease to move further right)
int midPos = 80; // middle
int pos = midPos;
bool calibrated = false;

Servo myservo;

std::string cmdStr = "";

std::string gatePos = "middle";

void publish(const std::string& topic, const std::string& payload) {
    auto fullTopic = topic + "/" + GATE_ID;
    client.publish(fullTopic.c_str(), payload.c_str());
}

void subscribe(const std::string& topic) {
    auto fullTopic = topic + "/" + GATE_ID;
    client.subscribe(fullTopic.c_str());
}

void connect() {
    Serial.print("checking wifi...");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(1000);
    }
    Serial.println("\nconnected! Local IP: ");
    Serial.print(WiFi.localIP());

    Serial.print("\nconnecting...");
    auto clientId ="savadhan-nodemcu-gate-" + GATE_ID;
    while (!client.connect(clientId.c_str())) {
        Serial.print(".");
        delay(1000);
    }

    Serial.println("\nconnected!");

    publish("/heartbeat", "hello");
    subscribe("/gatecmd");
}

void messageReceived(String &topic, String &payload) {
    Serial.println("incoming: " + topic + " - " + payload);
    cmdStr = payload.c_str();
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

    digitalWrite(SERVO_ENABLE_PIN, HIGH);
    delay(10);
    
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
    digitalWrite(SERVO_ENABLE_PIN, LOW);
}

void writeStringToEEPROM(int* addr, const std::string& input) {
    for (size_t i=0; i < input.length(); ++i) {
        EEPROM.write(*addr, input[i]);
        (*addr)++;
    }
    EEPROM.write(*addr, '\0');
    (*addr)++;
}

std::string readStringFromEEPROM(int* addr, size_t maxlen) {
    std::string result;
    result.reserve(maxlen);

    for (size_t i=0; i < maxlen; ++i) {
        char ch = EEPROM.read(*addr);
        (*addr)++;
        if (ch == '\0') {
            return result;
        }
        result.push_back(ch);
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
    writeStringToEEPROM(&addr, CHKSUM_STR);
    writeStringToEEPROM(&addr, GATE_ID);
    EEPROM.put(addr, minPos); addr += sizeof(minPos);
    EEPROM.put(addr, maxPos); addr += sizeof(maxPos);
    EEPROM.commit();

    calibrated = true;
}

void setup() {
    Serial.begin(115200);
    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
    pinMode(SERVO_ENABLE_PIN, OUTPUT);
    myservo.attach(SERVO_PWM_PIN);
    
    digitalWrite(SERVO_ENABLE_PIN, HIGH);

    myservo.write(midPos);
    delay(500);
    gatePos = "middle";

    digitalWrite(SERVO_ENABLE_PIN, LOW);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    client.begin(MQTT_CONTROLLER_IP, net);
    client.onMessage(messageReceived);

    EEPROM.begin(512);
    int addr = 0;
    // +1 is important! When we wrote CHKSUM_STR, we occupied CHKSUM_STR.length()+1 
    // bytes (1 for the trailing \0 character). Hence make sure to read upto the \0
    // character again
    auto chksum = readStringFromEEPROM(&addr, CHKSUM_STR.length()+1);
    if (chksum == CHKSUM_STR) {
        GATE_ID = readStringFromEEPROM(&addr, 10);

        EEPROM.get(addr, minPos); addr += sizeof(minPos);
        Serial.print(minPos);

        EEPROM.get(addr, maxPos); addr += sizeof(maxPos);

        Serial.println("Reading from EEPROM:");
        Serial.print("gate_id = "); Serial.println(GATE_ID.c_str());
        Serial.print("minPos = "); Serial.println(minPos);
        Serial.print("maxPos = "); Serial.println(maxPos);

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

    if (cmdStr == "open" || cmdStr == "close") {
        openClose(cmdStr);
    } else if (cmdStr == "middle") {
        Serial.println("Processing middle cmd");
        if (gatePos != "middle") {
            moveServoTo(midPos, false, 15);
        }
        gatePos = "middle";
    } else if (cmdStr == "calibrate") {
        calibrate();
    } else if (cmdStr != "") {
        Serial.print("Invalid command: ");
        Serial.println(cmdStr.c_str());
        cmdStr = "";
    }

    if (cmdStr != "") {
        publish("/gateack", cmdStr.c_str());
        cmdStr = "";
    }

    auto millisNow = millis();
    if (millisNow - lastMillis > 3000) {
        std::ostringstream os;
        os << "{"
            << "\"gatePos\" : \"" << gatePos.c_str() << "\", "
            << "\"ipAddress\": \"" << WiFi.localIP().toString().c_str() << "\""
            << "}";
        auto status = os.str();
        publish("/heartbeat", status.c_str());
        lastMillis = millisNow;
    }
}