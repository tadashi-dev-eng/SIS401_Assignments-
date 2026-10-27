# SIS401 Programming Assignment 2 — IoT Data Pipeline

**Cloud Ingestion, Real-Time Dashboard and Remote Actuation**

| | |
|---|---|
| **Student** | Tashi Penjor |
| **Student ID** | 02230306 |
| **Module** | SIS401 — Smart Integrated Systems |
| **Module Tutor** | Darshan Subedi |
| **Programme** | BE Software Engineering, College of Science and Technology |

---

## 1. Overview

This project implements an end-to-end IoT data pipeline. An ESP32 reads temperature, humidity and ambient light and publishes them as JSON over MQTT to a Mosquitto broker on a Raspberry Pi. Node-RED on the Pi displays the data on a real-time dashboard and forwards it to ThingsBoard in the cloud, where a high-temperature alarm is configured. A physical LED is controlled remotely from the dashboard (on/off, brightness, auto mode), and the ESP32 publishes the LED's actual state back to the broker on an acknowledgement topic.

| Component | Choice |
|---|---|
| Microcontroller | ESP32 DevKit V1 |
| Sensors | DHT22 (temperature + humidity), LDR module (light) |
| Actuator | LED with 220 Ω resistor (PWM dimmable) |
| Broker | Mosquitto 2.x on Raspberry Pi (password authentication) |
| Cloud platform | ThingsBoard |
| Dashboard | Node-RED Dashboard 2.0 (`@flowfuse/node-red-dashboard`) |
| Protocol | MQTT (device ↔ broker ↔ cloud) |
| Network | Mobile hotspot (ESP32, Pi and laptop on the same network) |

---

## 2. System Architecture

```
 ┌────────────────────────────┐
 │ ESP32                      │
 │  DHT22 ─ GPIO 4            │   MQTT telemetry (JSON, every 5 s)
 │  LDR   ─ GPIO 34           │ ───────────────────────────────┐
 │  LED   ─ GPIO 18           │ ◄──── LED command ───────────┐ │
 └────────────────────────────┘ ───── LED state (ACK) ─────┐ │ │
                                                           ▼ │ ▼
                                  ┌───────────────────────────────┐
                                  │ Raspberry Pi                  │
                                  │  Mosquitto broker :1883       │
                                  │  Node-RED :1880               │
                                  │   ├─ Dashboard 2.0            │
                                  │   │   charts, gauge, switch,  │
                                  │   │   slider, auto button     │
                                  │   └─ ThingsBoard forwarder ───┼──┐
                                  └───────────────────────────────┘  │ MQTT + device access token
                                                                     ▼
                                                   ┌───────────────────────────────┐
                                                   │ ThingsBoard (cloud)           │
                                                   │  Device: esp32-01             │
                                                   │  Latest telemetry view        │
                                                   │  Alarm: temperature > 30 °C   │
                                                   └───────────────────────────────┘
```

### Data flow

1. The ESP32 reads both sensors every 5 seconds and publishes a single JSON message to `cst/sis401/esp32-01/sensors/env`.
2. Node-RED subscribes to that topic and sends the data down two paths:
   - **Dashboard:** a function node splits the message into temperature, humidity and light, which drive two line charts and a gauge.
   - **Cloud:** a function node removes invalid readings and non-telemetry fields, then publishes to ThingsBoard on `v1/devices/me/telemetry` using the device access token.
3. ThingsBoard stores the telemetry and evaluates the alarm rule on every new value.
4. Dashboard controls publish commands to `cst/sis401/esp32-01/actuator/led/cmd`. The ESP32 validates each command, applies it to the LED and publishes the resulting state to `.../actuator/led/state`, which the dashboard displays as the confirmed state.

---

## 3. Hardware and Wiring

| Component | Pin | ESP32 pin | Notes |
|---|---|---|---|
| DHT22 | VCC | 3V3 | |
| DHT22 | DATA | GPIO 4 | Module includes pull-up resistor |
| DHT22 | GND | GND | |
| LDR module | VCC | 3V3 | 3.3 V keeps the output within the ADC range |
| LDR module | AO | GPIO 34 | ADC1 pin — ADC2 pins cannot be used while WiFi is active |
| LDR module | GND | GND | |
| LED | Anode (long leg) | GPIO 18 via 220 Ω | PWM brightness control |
| LED | Cathode (short leg) | GND | |

---

## 4. MQTT Design

### Topic hierarchy

Topics follow `<organisation>/<module>/<device>/<category>/<item>`, so more devices can be added and subscribed to with wildcards (e.g. `cst/sis401/+/sensors/env`).

| Topic | Direction | Retained | Purpose |
|---|---|---|---|
| `cst/sis401/esp32-01/sensors/env` | ESP32 → broker | No | Sensor telemetry |
| `cst/sis401/esp32-01/actuator/led/cmd` | Dashboard → ESP32 | No | LED command |
| `cst/sis401/esp32-01/actuator/led/state` | ESP32 → broker | Yes | Actuator acknowledgement |
| `cst/sis401/esp32-01/status` | ESP32 / broker | Yes | `online` / `offline` (Last Will) |

### Payloads

**Telemetry**
```json
{"device":"esp32-01","seq":10,"temperature":29.2,"humidity":47.9,"light":19,"rssi":-20}
```
If the DHT22 read fails, the message contains `"dht_error": true` instead of temperature and humidity, so no invalid values are sent. `seq` increments with each message so dropped messages can be detected.

**Commands**
```json
{"state":"on"}        {"state":"off"}
{"brightness":60}
{"mode":"auto"}       {"mode":"manual"}
```

**Acknowledgement**
```json
{"on":true,"brightness":60,"mode":"manual","source":"remote","uptime_s":642}
```
`source` is `boot`, `remote`, `auto` or `mode_change`, showing what caused the change.

### Broker security

Anonymous access is disabled on Mosquitto; the ESP32 and Node-RED log in with a username and password stored in `/etc/mosquitto/passwd`. WiFi and MQTT credentials are replaced with placeholders in the committed code, and the ThingsBoard access token is kept in Node-RED's encrypted credentials file, which is not exported with `flows.json`.

---

## 5. Firmware (`firmware/esp32_pipeline/esp32_pipeline.ino`)

| Function | Responsibility |
|---|---|
| `connectWiFi()` | Joins WiFi; called again from `loop()` if the connection drops |
| `connectMQTT()` | Authenticated connection with Last Will, subscribes to the command topic, publishes `online` and the current LED state |
| `publishSensors()` | Reads DHT22 and LDR, builds the JSON payload, publishes telemetry |
| `onMessage()` | Parses and validates commands, applies them, triggers acknowledgement |
| `setLed()` | Sets LED brightness 0–100 % with PWM |
| `runAutoMode()` | Night-light behaviour: darker room → brighter LED |
| `publishLedState()` | Publishes the retained actuator state |

### Extensions beyond the Practical 5 and 6 code

1. **Three command types** — on/off, dimmable brightness (PWM) and mode selection — instead of a single value.
2. **Automatic night-light mode** using the LDR, overridden by any manual command.
3. **Input validation**: malformed JSON, unknown commands and out-of-range values are rejected and logged.
4. **Authenticated broker** instead of anonymous access.
5. **Last Will and Testament**, so the dashboard shows `offline` if the ESP32 disconnects unexpectedly.
6. **Retained acknowledgement** containing state, brightness, mode and the source of the change.
7. **Non-blocking timing** with `millis()` instead of `delay(5000)`, so commands apply immediately.
8. **Sensor error handling** with a `dht_error` flag instead of publishing `NaN`.
9. **Diagnostics**: sequence number and WiFi signal strength (RSSI) in every message.
10. **Automatic WiFi and MQTT reconnection**.

---

## 6. Cloud Platform — ThingsBoard

### Device registration
- Device `esp32-01` created under **Entities → Devices**.
- Authenticated with its **access token**, used as the MQTT username by the Node-RED forwarder.

### Telemetry forwarding
The Node-RED node **Format for ThingsBoard** drops readings flagged with `dht_error`, keeps `temperature`, `humidity`, `light` and `rssi`, and publishes them to `v1/devices/me/telemetry`. The data appears under the device's **Latest telemetry** tab.

### Alarm rule
Configured in the device profile under **Alarm rules**:

| Field | Value |
|---|---|
| Alarm type | `High Temperature` |
| Create condition | `temperature > 30` → **Critical** |
| Clear condition | `temperature <= 28` |

The 2 °C gap between the thresholds stops the alarm from repeatedly switching on and off when the temperature hovers around 30 °C. It was tested by warming the DHT22 by hand.

![Cloud telemetry](cloud_screenshot.png)

---

## 7. Real-Time Dashboard — Node-RED Dashboard 2.0

Available at `http://<pi-ip>:1880/dashboard`.

| Widget | Group | Description |
|---|---|---|
| Line chart | Live Sensor Data | Temperature (°C), updates every 5 s |
| Line chart | Live Sensor Data | Humidity (%), updates every 5 s |
| Gauge | Live Sensor Data | Light level (%) |
| Switch | LED Control | LED on/off |
| Slider (0–100) | LED Control | LED brightness |
| Button | LED Control | Switch to automatic night-light mode |
| Text | LED Control | LED state **confirmed by the ESP32** |
| Text | LED Control | Device online/offline status |

The confirmed-state widget is driven by the ESP32's acknowledgement, not by the dashboard controls, so it shows what the hardware actually did.

![Dashboard](dashboard_screenshot.png)

---

## 8. Setup Instructions

### 8.1 Network
Connect the Raspberry Pi, the ESP32 and the laptop to the same network. With an iPhone hotspot, enable **Maximize Compatibility** so it uses 2.4 GHz, which the ESP32 requires.

### 8.2 Raspberry Pi — MQTT broker
```bash
sudo apt update
sudo apt install -y mosquitto mosquitto-clients
sudo mosquitto_passwd -c /etc/mosquitto/passwd esp32
sudo cp broker/mosquitto.conf /etc/mosquitto/conf.d/sis401.conf
sudo systemctl restart mosquitto
hostname -I        # note the Pi's IP address
```

Check that messages arrive:
```bash
mosquitto_sub -h localhost -u esp32 -P <password> -t 'cst/#' -v
```

### 8.3 Raspberry Pi — Node-RED
```bash
bash <(curl -sL https://github.com/node-red/linux-installers/releases/latest/download/update-nodejs-and-nodered-deb)
sudo systemctl enable --now nodered
cd ~/.node-red && npm install @flowfuse/node-red-dashboard
node-red-restart
```
1. Open `http://<pi-ip>:1880`.
2. **☰ → Import** → select `node-red/flows.json` → **Import**.
3. Edit the **Pi Mosquitto** broker → **Security** → enter the MQTT username and password.
4. Edit the **ThingsBoard Cloud** broker → set the server to your ThingsBoard MQTT host → **Security** → Username = device access token.
5. **Deploy**. All MQTT nodes should show **connected**.

### 8.4 ThingsBoard
1. Create device `esp32-01` and copy its access token.
2. Add the alarm rule from Section 6 to the device profile.

### 8.5 ESP32 firmware
1. Arduino IDE: install the **esp32** board package (Espressif).
2. Install libraries: **PubSubClient**, **ArduinoJson** (v7), **DHT sensor library** (Adafruit).
3. Open `firmware/esp32_pipeline/esp32_pipeline.ino` and set the WiFi credentials, MQTT password and `MQTT_BROKER` (Pi IP).
4. Select **ESP32 Dev Module** and the correct COM port, then upload.
5. Open the Serial Monitor at 115200 baud and check that telemetry is being published.

### 8.6 Verification
```bash
# Telemetry every 5 s
mosquitto_sub -h <pi-ip> -u esp32 -P <password> -t 'cst/sis401/#' -v

# Manual LED command
mosquitto_pub -h <pi-ip> -u esp32 -P <password> \
  -t cst/sis401/esp32-01/actuator/led/cmd -m '{"state":"on"}'
```
The LED turns on and an acknowledgement appears on `.../actuator/led/state`.

---

## 9. Repository Structure

```
SIS401_Assignment2_02230306/
├── firmware/esp32_pipeline/esp32_pipeline.ino
├── broker/mosquitto.conf
├── node-red/flows.json
├── dashboard_screenshot.png
├── cloud_screenshot.png
├── README.md
├── REFLECTION.md
└── .gitignore
```

---

## 10. Requirements Checklist

| Requirement | How it is met |
|---|---|
| **FR1** Two sensors, MQTT, structured topics | DHT22 + LDR, JSON every 5 s on `cst/sis401/esp32-01/sensors/env` |
| **FR2** Cloud integration | ThingsBoard device with access token, live telemetry, high-temperature alarm rule |
| **FR3** Real-time dashboard | Node-RED Dashboard 2.0: temperature and humidity line charts plus light gauge |
| **FR4** Remote actuation | Physical LED controlled from dashboard switch, slider and button; state acknowledged on `.../actuator/led/state` |
| **Deployment** | Code, README, screenshots and reflection committed to GitHub |

---

## 11. Known Limitations

- MQTT runs over plain TCP on port 1883 without TLS.
- The LED can only be controlled from the local dashboard, not from ThingsBoard.
- The light reading is a relative percentage, not calibrated to lux.