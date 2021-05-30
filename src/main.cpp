#include <cstdio>
#include <Servo.h>
#include <ESP8266WiFi.h>
#include <MQTT.h>

#include "credentials.hpp"

WiFiClient net;
MQTTClient client;

const String GATE_ID = "6";

const int LIMIT_SWITCH_PIN = 5;
const int SERVO_PIN = 2;

const int delayT = 15;
const int maxPos = 125; // close (increase to move further left)
const int minPos = 25; // open (decrease to move further right)
const int midPos = 80; // middle
int pos = midPos;

Servo myservo;

enum class GateCmd {
    OPEN,
    CLOSE,
    MIDDLE,
    NONE  
};
GateCmd gateCmd = GateCmd::NONE;

enum class GatePos {
    OPENED,
    CLOSED,
    MIDDLE
};
GatePos gatePos = GatePos::MIDDLE;

void connect() {
    Serial.print("checking wifi...");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(1000);
    }

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

    auto& gateCmdStr = payload;
    if (gateCmdStr == "open") {
        gateCmd = GateCmd::OPEN;
    } else if (gateCmdStr == "close") {
        gateCmd = GateCmd::CLOSE;
    } else if (gateCmdStr == "middle") {
        gateCmd = GateCmd::MIDDLE;
    } else {
        Serial.println("Invalid gatecmd");
    }
}


void printState(int pos, int buttonState) {
    char buf[128];
    snprintf(buf, 128, "buttonState = %d, pos = %d\n", buttonState, pos);
    Serial.print(buf);
}

void moveServo(int pos) {
    myservo.write(pos);
    // delay to let servo reach the position
    delay(delayT);
}

void moveServoTo(int finalPos, bool check) {
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
        moveServo(pos);
        buttonState = digitalRead(LIMIT_SWITCH_PIN);
        printState(pos, buttonState);
        if (check && !buttonState) {
            break;
        }
    }

    if (check && !buttonState) {
        pos -= dpos;
        moveServo(pos);
        buttonState = digitalRead(LIMIT_SWITCH_PIN);
        printState(pos, buttonState);
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
    myservo.attach(SERVO_PIN);
    
    moveServo(midPos);
    gatePos = GatePos::MIDDLE;

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    client.begin(MQTT_CONTROLLER_IP, net);
    client.onMessage(messageReceived);

    connect();
}

unsigned long lastMillis = 0;

void loop() {
    if (!client.connected()) {   
        connect();    
    } else {
        client.loop();
        delay(10);  // <- fixes some issues with WiFi stability
    }

    if (gateCmd == GateCmd::OPEN) {
        Serial.println("Processing open cmd");
        if (gatePos != GatePos::OPENED) {
            moveServoTo(midPos, false);
            moveServoTo(minPos, true);
        }
        gateCmd = GateCmd::NONE;
        gatePos = GatePos::OPENED;
        client.publish("/gateack/" + GATE_ID, "opened");
    } else if (gateCmd == GateCmd::CLOSE) {
        Serial.println("Processing close cmd");
        if (gatePos != GatePos::CLOSED) {
            moveServoTo(midPos, false);
            moveServoTo(maxPos, true);
        }
        gateCmd = GateCmd::NONE;
        gatePos = GatePos::CLOSED;
        client.publish("/gateack/" + GATE_ID, "closed");
    } else if (gateCmd == GateCmd::MIDDLE) {
        Serial.println("Processing middle cmd");
        if (gatePos != GatePos::MIDDLE) {
            moveServoTo(midPos, false);
        }
        gateCmd = GateCmd::NONE;
        gatePos = GatePos::MIDDLE;
        client.publish("/gateack/" + GATE_ID, "middled");
    }

    auto millisNow = millis();
    if (millisNow - lastMillis > 3000) {
        client.publish("/heartbeat/" + GATE_ID, "tick");
        lastMillis = millisNow;
    }
}