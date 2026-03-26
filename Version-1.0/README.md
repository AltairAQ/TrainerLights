# TrainerLights

**A WiFi-based reflex training system for athletes**  
Measure and improve reaction time using wireless sensor nodes.

##  Table of Contents
1. [Overview](#overview)
2. [Features](#features)
3. [Hardware Requirements](#hardware-requirements)
4. [Software Requirements](#software-requirements)
5. [Wiring Diagrams](#wiring-diagrams)
6. [Getting Started](#getting-started)
7. [Using the Web Interface](#using-the-web-interface)
8. [Client Button Functions](#client-button-functions)
9. [Configuration Storage](#configuration-storage)
10. [Statistics Explained](#statistics-explained)
11. [Troubleshooting](#troubleshooting)

---

##  Overview

TrainerLights is an open-source, low-cost reflex training system. One **server** (ESP8266) creates a WiFi access point and multiple **client lights** (ESP8266 + HC-SR04 sensor) connect to it.

When the active client turns on, the athlete moves into its detection zone. The system measures reaction time and distance, updates stats in real time, and exposes a coach-facing web interface.

---

##  Features

- Server-based WiFi network (TrainerLights) to coordinate clients
- Mobile-friendly web dashboard with real-time stats
- Reaction/distance tracking (avg/min/max)
- Configurable delays, timeouts, distance ranges
- Random or sequential stimulus modes
- Up to 32 client nodes
- Sensor health tracking with timeout and disconnect logic
- EEPROM-based persistent settings
- Local button controls on each client (short/medium/long press)
- Distance hysteresis and moving average for stable detection

---

##  Hardware Requirements

### Server (1x ESP8266)
- NodeMCU ESP8266 (or equivalent)
- USB power cable
- Optional status LED (built-in D4)

### Client (up to 32)
- NodeMCU ESP8266
- HC-SR04 ultrasonic sensor
- 3 LEDs (stimulus, success, error) + 220330Ω resistors
- 1 tactile push button (GND pull-up)
- Jumper wires
- Breadboard/PCB

> Pin mappings are in code, verify before wiring.

---

##  Software Requirements

### Arduino IDE / PlatformIO
Install these libraries:
- ESP8266WiFi (built-in)
- WebSockets by Markus Sattler
- ArduinoJson (v6)
- TaskScheduler by Anatoli Arkhipenko
- LinkedList by Ivan Seidel
- EEPROM (built-in)

### ESP8266 board support
1. Go to File  Preferences
2. Add http://arduino.esp8266.com/stable/package_esp8266com_index.json
3. Open Tools  Board  Board Manager, install esp8266

---

##  Wiring Diagrams

### Client Node (per light)

| Component | ESP8266 Pin |
|---|---|
| HC-SR04 VCC | 5V or 3.3V |
| HC-SR04 GND | GND |
| HC-SR04 TRIG | D1 |
| HC-SR04 ECHO | D2 |
| Stimulus LED | D6 (resistor) |
| Success LED | D0 (resistor) |
| Error LED | D7 (resistor) |
| Button (GND side) | D5 (INPUT_PULLUP) |
| On-board LED | D4 |

> Use a current-limiting resistor (220Ω) for each LED.

### Server Node
No external trigger sensor needed. Only built-in D4 LED status.

---

##  Getting Started

### 1. Prepare the Server
1. Open TrainerLights_server.ino
2. Select NodeMCU 1.0 (ESP-12E) and correct COM port
3. Upload sketch
4. Open Serial Monitor at 115200 baud, note 192.168.4.1

### 2. Prepare Clients
1. Open TrainerLights_client.ino
2. Adjust pins if needed
3. Upload to each client
4. Each client connects to TrainerLights AP

### 3. Power Up
- Power server and clients via USB (or battery for clients)
- Client LED blink pattern indicates network connection

### 4. Connect Controller Device
1. Connect phone/tablet to TrainerLights (password 1234567890)
2. Open browser: http://192.168.4.1 (or http://trainerlights.local)
3. Use control panel to start the session

---

##  Using the Web Interface

### Sections
- Timer: start/stop/reset
- Scores & errors
- Reaction/distance stats
- Mode: random/sequential
- Delay/time-out/detection range
- Configure button (EEPROM save)
- List sensors

### Suggested Workflow
1. Set parameters
2. Configure
3. Start
4. Athlete reacts to active lights
5. Monitor stats
6. Stop

---

##  Client Button Functions

| Duration | Action | Feedback | Use Case |
|---|---|---|---|
| <1s | Toggle status LED | Built-in LED on/off | identify node |
| 13s | Manual stimulus | Stimulus LED on | local test |
| >3s | Calibration | red+green (yellow) | range calibration |

Calibration performs 10 readings and displays average distance; >100 cm triggers warning.

---

##  Configuration Storage

Server EEPROM stores:
- mode (random/sequential)
- delay min/max
- timeout min/max
- detection range min/max

Settings persist across reboots.

---

##  Statistics Explained

- **Score**: successful hits
- **Errors**: timeouts/false triggers
- **Reaction time**: ms from LED on to detection
- **Distance**: measured range at hit

Stats reset on Start.

---

##  Troubleshooting

### Clients won't connect
- Server AP running and visible?
- Correct password (1234567890)?
- Check client serial logs

### Web page not loading
- Device on TrainerLights WiFi
- Try http://192.168.4.1
- Check server serial errors

### Sensors not detecting
- Verify HC-SR04 wiring
- Run calibration (long press)
- Adjust detection range in UI

### Erratic readings
- Code uses moving-average + two-consecutive detections
- Tune thresholds in code if needed
