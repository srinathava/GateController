#include <cstdio>
#include <sstream>
#include <Servo.h>
#include <ESP8266WiFi.h>
#include <MQTT.h>
#include <EEPROM.h>

#include "credentials.hpp"

WiFiClient net;
MQTTClient client;

std::string GATE_ID = "4";

const int LIMIT_SWITCH_PIN = D1;
const int SERVO_PWM_PIN = D4;
const int SERVO_ENABLE_PIN = D8;

// Changing this string will invalidate the existing contents.
const std::string CHKSUM_STR = "eeprom_init_v9";

int closedPos = 100; // close (increase to move further left)
int openPos = 20; // open (decrease to move further right)

Servo myservo;

std::string cmdStr = "";

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
    cmdStr = payload.c_str();
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
    delay(500);
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
        myservo.write(closedPos);
    } else {
        myservo.write(openPos);
    }
    delay(500);
    digitalWrite(SERVO_ENABLE_PIN, LOW);
    gatePos = finalPos;
}

void loop() {
    if (!client.connected()) {   
        connect();    
    } else {
        client.loop();
        delay(10);  // <- fixes some issues with WiFi stability
    }
    
    if (cmdStr == "open" || cmdStr == "close") {
        openClose(cmdStr);
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