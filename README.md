# Blast Gate Controller

This code is for controlling a servo motor using ESP 8266 via MQTT commands sent from a central gate coordinator. 
This is part of an overall set of projects for automatically opening/closing dust collection blast gates for my 
woodworking workshop.

The code is meant to run on the following circuit schematic:

https://oshwlab.com/srinathava/try1

![Gate Controller Schematic](Schematic_GateController_2022-12-27.svg "Gate Controller Schematic")

NOTE: Use VSCode with the Platform IO extension to compile the project. Note that you will need to
create a file called credentials.hpp with the following contents in order to supply WIFI credentials:

```c
#define WIFI_SSID "MyNetWorkSSID"
#define WIFI_PASS "MyNetworkPassword"
#define MQTT_CONTROLLER_IP "192.168.4.1"
const std::string OTA_UPDATE_IP = "http://192.168.4.1:5000/update";
```

Place `credentials.hpp` in the same folder as `main.cpp`.
