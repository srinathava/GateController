#include <cstdio>
#include <sstream>
#include <Servo.h>
#include <ESP8266WiFi.h>
#include <MQTT.h>
#include <EEPROM.h>

#include "credentials.hpp"

WiFiClient net;
MQTTClient client;

std::string GATE_ID = "10";

const int LIMIT_SWITCH_PIN = D1;
const int SERVO_PWM_PIN = D4;
const int SERVO_ENABLE_PIN = D8;
const int SERVO_MOVE_TIME_MS = 1000;

// Changing this string will invalidate the existing contents.
const std::string CHKSUM_STR = "eeprom_init_v1";

int closedPos = 20;
int openPos = 110;

Servo myservo;

std::string topicStr = "";
std::string payloadStr = "";

std::string gatePos = "close";

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
    topicStr = topic.c_str();
    // Remove the final substring like "/10"
    topicStr = topicStr.substr(0, topicStr.size() - GATE_ID.size() - 1);
    payloadStr = payload.c_str();
}

void printState(int pos, int buttonState) {
    char buf[128];
    snprintf(buf, 128, "buttonState = %d, pos = %d\n", buttonState, pos);
    Serial.print(buf);
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

void setup() {
    Serial.begin(115200);
    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
    pinMode(SERVO_ENABLE_PIN, OUTPUT);
    myservo.attach(SERVO_PWM_PIN);
    
    digitalWrite(SERVO_ENABLE_PIN, HIGH);

    myservo.write(closedPos);
    delay(SERVO_MOVE_TIME_MS);
    gatePos = "close";

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

        EEPROM.get(addr, openPos); addr += sizeof(openPos);
        Serial.print(openPos);

        EEPROM.get(addr, closedPos); addr += sizeof(closedPos);

        Serial.println("Reading from EEPROM:");
        Serial.print("gate_id = "); Serial.println(GATE_ID.c_str());
        Serial.print("openPos = "); Serial.println(openPos);
        Serial.print("closePos = "); Serial.println(closedPos);
    } else {
        Serial.print("Uninitialized data in EEPROM: ");
        Serial.println(chksum.c_str());
    }

    connect();
}

unsigned long lastMillis = 0;

void openClose(const std::string& finalPos) {
    Serial.print("Processing move cmd: ");
    Serial.println(finalPos.c_str());

    if (gatePos == finalPos)
        return;
    
    digitalWrite(SERVO_ENABLE_PIN, HIGH);
    if (finalPos == "open") {
        myservo.write(openPos);
    } else if (finalPos == "close") {
        myservo.write(closedPos);
    } else if (finalPos == "middle") {
        auto midPos = (openPos + closedPos)/2;
        myservo.write(midPos);
    }
    delay(SERVO_MOVE_TIME_MS);
    digitalWrite(SERVO_ENABLE_PIN, LOW);
    gatePos = finalPos;
}

void setLimits() {
    try {
        int& target = (topicStr == "gatemin") ? closedPos : openPos;
        target = std::stoi(payloadStr);
        Serial.printf("Setting %s = %d\n", topicStr.c_str(), target);
    } catch (std::invalid_argument) {
        Serial.printf("Invalid command: %s %s\n", topicStr.c_str(), payloadStr.c_str());
        return;
    }

    EEPROM.begin(512);
    int addr = 0;
    writeStringToEEPROM(&addr, CHKSUM_STR);
    writeStringToEEPROM(&addr, GATE_ID);
    EEPROM.put(addr, closedPos); addr += sizeof(closedPos);
    EEPROM.put(addr, openPos); addr += sizeof(openPos);
    EEPROM.commit();
}

void loop() {
    if (!client.connected()) {   
        connect();    
    } else {
        client.loop();
        delay(10);  // <- fixes some issues with WiFi stability
    }
    
    if (topicStr == "gatecmd") {
        openClose(payloadStr);
    } else if (topicStr == "gatemin" || topicStr == "gatemax") {
        setLimits();
    } else if (payloadStr != "") {
        Serial.print("Invalid command: ");
        Serial.println(payloadStr.c_str());
        payloadStr = "";
    }

    if (payloadStr != "") {
        publish("/gateack", payloadStr.c_str());
        payloadStr = "";
    }

    auto millisNow = millis();
    if (millisNow - lastMillis > 3000) {
        std::ostringstream os;
        os << "{"
            << "\"gatePos\" : \"" << gatePos.c_str()
            << ", \"openPos\": " << openPos
            << ", \"closedPos\": " << closedPos
            << "}";
        auto status = os.str();
        publish("/heartbeat", status.c_str());
        lastMillis = millisNow;
    }
}