#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Sensor library includes
#include <DHT.h>                   // DHT22 (digital single-wire)

// HTTP REST Web Server & JSON Libraries
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// Wireless Configuration
const char* WIFI_SSID     = "CST-RKB";
const char* WIFI_PASSWORD = "Hostel@RKB#";

// Pin Definitions
#define DHT_PIN     4            // Digital sensor pin
#define DHT_TYPE    DHT22
#define LDR_PIN     34           // Analog ADC1 pin (GPIO34)
#define LED_PIN     2            // Actuator pin

// MPU6050 / Clone Register Definitions
#define MPU_ADDR         0x68
#define MPU_PWR_MGMT_1   0x6B
#define MPU_ACCEL_XOUT_H 0x3B

// Hardware objects
Adafruit_SSD1306 display(128, 64, &Wire, -1);
DHT              dht(DHT_PIN, DHT_TYPE);
WebServer        server(80);     // Web Server listening on port 80

// Global App States
unsigned long startTime;
bool ledState = false;           // Tracks manual POST /led instructions

// Visual Alarm Priority Enumeration Tracker
enum AlertType { NONE, NIGHTLIGHT, MOTION, TEMPERATURE };
AlertType currentAlert = NONE;

// Non-blocking background scheduler trackers
unsigned long lastDisplayTime = 0;
const unsigned long DISPLAY_INTERVAL = 500; // Update OLED screen matrix every 500ms
unsigned long lastBlinkTime = 0;
bool blinkState = false;

// Global Sensor Variables
float tempC = NAN;
float humPct = NAN;
int   lightRaw = 0;
float lightPct = 0.0;            // Computed Light Percentage
float accelX = 0.0, accelY = 0.0, accelZ = 0.0;
bool  dhtOk = false;
bool  mpuOk = false;

// Function Prototypes
void readSensors();
void updateDisplay();
void handleGetSensors();
void handlePostLed();
void handleGetStatus();
bool initMPUDirect();

void setup() {
  Serial.begin(115200);
  delay(200);

  // Initialize I2C Bus (SDA = GPIO21, SCL = GPIO22)
  Wire.begin(21, 22);

  // Initialize OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found"); 
    while (true);
  }
  display.clearDisplay(); 
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Continuous API Hub..."); 
  display.display();

  // Initialize digital and analog sensors
  dht.begin();
  Serial.println("DHT22 initialized.");

  analogReadResolution(12);
  pinMode(LDR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  Serial.println("Sensors & Actuators initialized.");

  // Direct register wake-up for MPU clone chip (bypasses WHO_AM_I check)
  if (initMPUDirect()) {
    mpuOk = true;
    Serial.println("MPU6050 (Direct Register Mode) initialized at address 0x68.");
  } else {
    mpuOk = false;
    Serial.println("Failed to find MPU6050 chip!");
  }

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  startTime = millis();
  Serial.println("\nServer at: http://" + WiFi.localIP().toString());

  // Define REST API Routing Routes
  server.on("/sensors", HTTP_GET, handleGetSensors);
  server.on("/led", HTTP_POST, handlePostLed);
  server.on("/status", HTTP_GET, handleGetStatus);
  
  // 404 Catch-All Routing Rule
  server.onNotFound([](){
    server.send(404, "application/json", "{\"error\":\"Not found\",\"endpoints\":[\"/sensors\",\"/led\",\"/status\"]}");
  });

  server.begin();
  Serial.println("Endpoints active: GET /sensors | POST /led | GET /status");
}

void loop() {
  server.handleClient(); // Instantly process incoming Postman/Browser actions

  // 1. Monitor hardware environments continuously
  readSensors();

  // 2. Continuous Threat Analysis Hierarchy (Calibrated for baseline)
  if (mpuOk && (abs(accelX - 10.0) > 3.0 || abs(accelY) > 3.0)) {
    currentAlert = MOTION;      // Top Priority: Fast LED Flash (100ms)
  } 
  else if (dhtOk && tempC > 33.0) {
    currentAlert = TEMPERATURE; // Secondary Priority: Slow LED Flash (500ms)
  } 
  else if (lightPct < 35.0) { 
    currentAlert = NIGHTLIGHT;  // Tertiary Priority: Solid LED ON (< 35% brightness)
  } 
  else {
    currentAlert = NONE;        // Idle: Follows manual Postman POST /led state
  }

  // 3. Execution of Modulated LED Pulse Signatures
  switch (currentAlert) {
    case MOTION:
      if (millis() - lastBlinkTime >= 100) {
        lastBlinkTime = millis();
        blinkState = !blinkState;
        digitalWrite(LED_PIN, blinkState ? HIGH : LOW);
      }
      break;

    case TEMPERATURE:
      if (millis() - lastBlinkTime >= 500) {
        lastBlinkTime = millis();
        blinkState = !blinkState;
        digitalWrite(LED_PIN, blinkState ? HIGH : LOW);
      }
      break;

    case NIGHTLIGHT:
      digitalWrite(LED_PIN, HIGH);
      break;

    case NONE:
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
      break;
  }

  // 4. Update physical OLED layout safely on an interval to prevent flickering
  if (millis() - lastDisplayTime >= DISPLAY_INTERVAL) {
    lastDisplayTime = millis();
    updateDisplay();
  }
}

// Low-level register wake-up
bool initMPUDirect() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(MPU_PWR_MGMT_1);
  Wire.write(0x00); // Wake up sensor from sleep mode
  return (Wire.endTransmission() == 0);
}

void readSensors() {
  // DHT22 Read
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  
  if (!isnan(t) && !isnan(h)) {
    tempC = t;
    humPct = h;
    dhtOk = true;
  } else {
    dhtOk = false;
  }

  // LDR Read & Percentage Conversion (0% = Dark, 100% = Bright)
  lightRaw = analogRead(LDR_PIN);
  lightPct = (1.0 - ((float)lightRaw / 4095.0)) * 100.0;
  if (lightPct < 0.0) lightPct = 0.0;
  if (lightPct > 100.0) lightPct = 100.0;

  // MPU6050 Acceleration Read via I2C directly
  if (mpuOk) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(MPU_ACCEL_XOUT_H);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)6);

    if (Wire.available() >= 6) {
      int16_t rawX = (Wire.read() << 8) | Wire.read();
      int16_t rawY = (Wire.read() << 8) | Wire.read();
      int16_t rawZ = (Wire.read() << 8) | Wire.read();

      accelX = (rawX / 16384.0) * 9.81;
      accelY = (rawY / 16384.0) * 9.81;
      accelZ = (rawZ / 16384.0) * 9.81;
    }
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // Row 1: Header Title
  display.setCursor(0, 0);
  display.println("=Group2=");

  // Row 2: Temperature Reading
  display.setCursor(0, 12);
  if (dhtOk) {
    display.printf("Temp : %.1f C", tempC);
  } else {
    display.print("Temp : -- C");
  }

  // Row 3: Humidity Reading
  display.setCursor(0, 22);
  if (dhtOk) {
    display.printf("Hum  : %.1f %%", humPct);
  } else {
    display.print("Hum  : -- %");
  }

  // Row 4: Light Intensity Percentage
  display.setCursor(0, 32);
  display.printf("Light: %.1f %%", lightPct);

  // Row 5: Accelerometer Metrics
  display.setCursor(0, 42);
  if (mpuOk) {
    display.printf("AccX : %.1f m/s2", accelX);
  } else {
    display.print("Acc  : Offline");
  }

  // Row 6: Active Alarm / System State
  display.setCursor(0, 54);
  display.print("Status : ");
  switch (currentAlert) {
    case MOTION:      display.println(" MOTION !"); break;
    case TEMPERATURE: display.println(" OVERHEAT !"); break;
    case NIGHTLIGHT:  display.println("NIGHTLIGHT"); break;
    case NONE:        display.println(ledState ? "LED ON" : "NORMAL"); break;
  }

  display.display();
}

// REST Endpoint: GET /sensors
void handleGetSensors() {
  StaticJsonDocument<384> doc;
  
  doc["temperature"]["value"] = dhtOk ? tempC : 0.0;
  doc["temperature"]["unit"]  = "celsius";
  
  doc["humidity"]["value"]    = dhtOk ? humPct : 0.0;
  doc["humidity"]["unit"]      = "percent";
  
  doc["light_percentage"]["value"] = String(lightPct, 1).toFloat();
  doc["light_percentage"]["unit"]  = "percent";
  doc["light_raw"]["value"]        = lightRaw;
  
  doc["accelerometer"]["x"]   = mpuOk ? accelX : 0.0;
  doc["accelerometer"]["y"]   = mpuOk ? accelY : 0.0;
  doc["accelerometer"]["z"]   = mpuOk ? accelZ : 0.0;

  String response;
  serializeJsonPretty(doc, response);
  server.send(200, "application/json", response);
}

// REST Endpoint: POST /led
void handlePostLed() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"No body\"}");
    return;
  }

  StaticJsonDocument<64> doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  
  if (error) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  String state = doc["state"] | "";
  if (state == "on") {
    ledState = true;
    server.send(200, "application/json", "{\"led\":\"on\"}");
  } else if (state == "off") {
    ledState = false;
    server.send(200, "application/json", "{\"led\":\"off\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"Use 'on' or 'off'\"}");
  }
}

// REST Endpoint: GET /status
void handleGetStatus() {
  StaticJsonDocument<256> doc;
  doc["device"]    = "SIS401 ESP32 Multi-Alert";
  doc["ip"]        = WiFi.localIP().toString();
  doc["uptime_s"]  = (millis() - startTime) / 1000;
  doc["led"]       = ledState ? "on" : "off";
  doc["wifi_rssi"] = WiFi.RSSI();

  String response;
  serializeJsonPretty(doc, response);
  server.send(200, "application/json", response);
}
