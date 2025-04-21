#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <MQTT.h>
#include <Servo.h>
#include <cstdarg>
#include <cstdio>
#include <sstream>

#include "credentials.hpp"

std::string GATE_ID = "11";

#define USE_WEMOS_D1_MINI
#ifdef USE_WEMOS_D1_MINI
const int SERVO_PWM_PIN = D3;
const int SERVO_ENABLE_PIN = D2;
#else
const int SERVO_PWM_PIN = D4;
const int SERVO_ENABLE_PIN = D8;
#endif

const int SERVO_MOVE_TIME_MS = 1000;

// Changing this string will invalidate the existing contents.
const std::string CHKSUM_STR = "2025-03-05-01";

WiFiClient net;
MQTTClient client;

int closePos = 20;
int openPos = 110;

Servo myservo;

std::string topicStr = "";
std::string payloadStr = "";

std::string gatePos = "";

void publish(const std::string &topic, const std::string &payload) {
    auto fullTopic = topic + "/" + GATE_ID;
    client.publish(fullTopic.c_str(), payload.c_str());
}

// Function to both print to serial and publish to MQTT
void logMessage(const char *format, ...) {
    // First, print to serial
    va_list args;
    va_start(args, format);
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Print to serial
    Serial.print(buffer);

    // Publish to MQTT
    publish("/gatelog", buffer);
}

void subscribe(const std::string &topic) {
    auto fullTopic = topic + "/" + GATE_ID;
    client.subscribe(fullTopic.c_str());
}

void connect() {
    if (WiFi.status() != WL_CONNECTED) {
        logMessage("Connecting...");
        while (WiFi.status() != WL_CONNECTED) {
            logMessage(".");
            delay(1000);
        }
        auto ipAddr = WiFi.localIP();
        auto ipAddrStr = ipAddr.toString();
        logMessage("\nIP address: %s\n", ipAddrStr.c_str());
    }

    auto clientId ="savadhan-nodemcu-gate-" + GATE_ID;
    while (!client.connect(clientId.c_str())) {
        delay(1000);
    }
    subscribe("/gatecmd");
    subscribe("/setopenpos");
    subscribe("/setclosepos");
    subscribe("/flash");
}

void messageReceived(String &topic, String &payload) {
    logMessage("incoming message: %s %s\n", topic.c_str(), payload.c_str());
    topicStr = topic.c_str();
    // Remove the initial "/" and the final substring like "/10"
    topicStr = topicStr.substr(1, topicStr.size() - GATE_ID.size() - 2);
    payloadStr = payload.c_str();
}

void writeStringToEEPROM(int *addr, const std::string &input) {
    for (size_t i = 0; i < input.length(); ++i) {
        EEPROM.write(*addr, input[i]);
        (*addr)++;
    }
    EEPROM.write(*addr, '\0');
    (*addr)++;
}

std::string readStringFromEEPROM(int *addr, size_t maxlen) {
    std::string result;
    result.reserve(maxlen);

    for (size_t i = 0; i < maxlen; ++i) {
        char ch = EEPROM.read(*addr);
        (*addr)++;
        if (ch == '\0') {
            return result;
        }
        result.push_back(ch);
    }
    return result;
}

void openClose(const std::string& finalPos) {
    logMessage("Processing move cmd: %s\n", finalPos.c_str());

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

void setup() {
    Serial.begin(115200);
    pinMode(SERVO_ENABLE_PIN, OUTPUT);
    myservo.attach(SERVO_PWM_PIN);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    client.begin(MQTT_CONTROLLER_IP, net);
    client.onMessage(messageReceived);

    EEPROM.begin(512);
    int addr = 0;
    // +1 is important! When we wrote CHKSUM_STR, we occupied
    // CHKSUM_STR.length()+1 bytes (1 for the trailing \0 character). Hence make
    // sure to read upto the \0 character again
    auto chksum = readStringFromEEPROM(&addr, CHKSUM_STR.length() + 1);
    if (chksum == CHKSUM_STR) {
        GATE_ID = readStringFromEEPROM(&addr, 10);
        EEPROM.get(addr, openPos);
        addr += sizeof(openPos);
        EEPROM.get(addr, closePos);
        addr += sizeof(closePos);
        logMessage(
            "Reading from EEPROM, gate+id = %s, openPos = %d, closePos = %d\n",
            GATE_ID.c_str(), openPos, closePos);
    } else {
        logMessage("Uninitialized data in EEPROM: %s\n", chksum.c_str());
    }

    connect();
    openClose("close");
}

unsigned long lastMillis = 0;

void setLimits() {
    // First check if the string is empty
    if (payloadStr.length() == 0) {
        logMessage("Invalid command: %s %s (empty string)\n", topicStr.c_str(), payloadStr.c_str());
        return;
    }

    // Check if all characters are digits
    int value = 0;
    for (auto c: payloadStr) {
        if (c < '0' || c > '9') {
            logMessage("Invalid command: %s %s (not a number)\n", topicStr.c_str(), payloadStr.c_str());
            return;
        }
        value = value*10 + (c - '0');
    }
    
    // Set the appropriate target
    int& target = (topicStr == "setclosepos") ? closedPos : openPos;
    target = value;
    logMessage("Setting %s = %d\n", topicStr.c_str(), target);
    gatePos = "";
    openClose(topicStr == "setclosepos" ? "close" : "open");
}

void loop() {
    if (!client.connected()) {
        connect();
    } else {
        client.loop();
        delay(10); // <- fixes some issues with WiFi stability
    }

    if (topicStr == "gatecmd") {
        openClose(payloadStr);
        publish("/gateack", payloadStr.c_str());
    } else if (topicStr == "setclosepos" || topicStr == "setopenpos") {
        setLimits();
        publish("/limitsack", topicStr.c_str());
    } else if (topicStr == "flash") {
        EEPROM.begin(512);
        int addr = 0;
        writeStringToEEPROM(&addr, CHKSUM_STR);
        writeStringToEEPROM(&addr, GATE_ID);
        EEPROM.put(addr, closePos);
        addr += sizeof(closePos);
        EEPROM.put(addr, openPos);
        addr += sizeof(openPos);
        EEPROM.commit();
        publish("/flashack", "done");
    } else if (payloadStr != "") {
        logMessage("Invalid command %s %s\n", topicStr.c_str(), payloadStr.c_str());
    }

    topicStr = "";
    payloadStr = "";

    auto millisNow = millis();
    if (millisNow - lastMillis > 3000) {
        std::ostringstream os;
        os << "{"
            << "\"gatePos\": \"" << gatePos.c_str() << "\""
            << ", \"openPos\": " << openPos
            << ", \"closedPos\": " << closedPos
            << "}";
        auto status = os.str();
        publish("/heartbeat", status.c_str());
        lastMillis = millisNow;
    }
}
