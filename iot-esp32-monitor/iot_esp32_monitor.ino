#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <MHZ19.h>
#include "driver/gpio.h" // Required for deep sleep GPIO hold

// --- Configuration ---
const char* ssid     = ""; 
const char* password = "";
#define mqtt_server "192.168.10.198" 
#define mqttDataTopic "growShed/sensorData"

// Deep Sleep Settings (30 minute)
#define min_to_sleep 15
uint64_t DEEP_SLEEP_TIME = (uint64_t)min_to_sleep * 60 * 1000000;

// --- Hardware Pins ---
#define RX_PIN 16                // MH-Z19B TX -> ESP32 RX2
#define TX_PIN 17                // MH-Z19B RX -> ESP32 TX2
const int moisturePin = 34;      // HD-38 Analog Output
const int sensorPowerPin = 25;   // HD-38 VCC Power Toggle
const int relayPin = 26;         // Relay Signal Pin (RTC capable)

// --- Humidifier Control Settings ---
const float humTurnOn  = 45.0;   // Turn ON humidifier if humidity drops below this
const float humTurnOff = 65.0;   // Turn OFF humidifier if humidity rises above this

// Store the relay state in RTC memory so it survives Deep Sleep
RTC_DATA_ATTR bool isHumidifierOn = false;

// --- Global Objects ---
WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_BME280 bme;
MHZ19 myMHZ19;

// --- Sensor Variables ---
float tempExt, humExt, pressExt;
int co2Value = 400;
int moistureValue = 1000;

void reconnect() {
  int attempt = 0;
  while (!client.connected() && attempt < 5) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("growTentController")) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" trying again in 2 seconds");
      attempt++;
      delay(2000);
    }
  }
  if (!client.connected()) ESP.restart(); 
}

void getValues() {
  // 1. Power up and Read Moisture (HD-38)
  pinMode(sensorPowerPin, OUTPUT);
  digitalWrite(sensorPowerPin, HIGH); 
  delay(50); 
  moistureValue = analogRead(moisturePin);
  digitalWrite(sensorPowerPin, LOW); 

  // 2. Read BME280
  tempExt = bme.readTemperature();
  humExt = bme.readHumidity();
  pressExt = bme.readPressure() / 100.0F; 

  // 3. Read MH-Z19B CO2
  co2Value = myMHZ19.getCO2();
  
  int try_count = 0;
  while (co2Value == 0 || co2Value==5000){
  
    Serial.println("CO2 read failed (0 or 5000 ppm). Retrying...");
    delay(180000); // Give the sensor a moment to reply
    co2Value = myMHZ19.getCO2();
    try_count++;
    if (try_count >= 3) {
      break;
    }
  }
  // Debug Print
  Serial.println("\n--- Readings ---");
  Serial.printf("Temp: %.2f C | Hum: %.2f %% | Press: %.2f hPa\n", tempExt, humExt, pressExt);
  Serial.printf("CO2: %d ppm | Moisture Raw: %d\n", co2Value, moistureValue);
}

void handleHumidifier() {
  // Logic with hysteresis
  if (humExt < humTurnOn) {
    isHumidifierOn = true;
    Serial.println("Humidity low: Humidifier ON");
  } else if (humExt > humTurnOff) {
    isHumidifierOn = false;
    Serial.println("Humidity good: Humidifier OFF");
  } else {
    Serial.printf("Humidity in range. Humidifier remains %s\n", isHumidifierOn ? "ON" : "OFF");
  }

  // Apply the state (Assuming Active HIGH relay.)
  digitalWrite(relayPin, isHumidifierOn ? HIGH : LOW);
}

void setup() {
  Serial.begin(9600);
  
  // --- Initialize Relay & Release GPIO Hold ---
  // Must un-hold the pin from the previous sleep cycle to change it
  gpio_hold_dis((gpio_num_t)relayPin);
  pinMode(relayPin, OUTPUT);
  // Re-apply previous state immediately upon waking
  digitalWrite(relayPin, isHumidifierOn ? HIGH : LOW); 

  // Initialize GPIOs
  pinMode(sensorPowerPin, OUTPUT);
  digitalWrite(sensorPowerPin, LOW);

  // Initialize CO2 Sensor
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  myMHZ19.begin(Serial2);
  myMHZ19.autoCalibration(true); 

  // Initialize BME280
  if (!bme.begin(0x76)) {
    Serial.println("Could not find BME280!");
  }

  // Connect Wi-Fi
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long wifiTimeout = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiTimeout < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    client.setServer(mqtt_server, 1883);

    // Get sensor data
    getValues();
    
    // Evaluate and set relay state based on new humidity reading
    handleHumidifier();

    // Connect and Publish
    if (!client.connected()) {
      reconnect();
    }

    // Construct JSON (Now includes relay state for your Node-RED dashboard!)
    String payload = "{";
    payload += "\"temperature\":" + String(tempExt) + ",";
    payload += "\"humidity\":" + String(humExt) + ",";
    payload += "\"pressure\":" + String(pressExt) + ",";
    payload += "\"moisture\":" + String(moistureValue) + ",";
    payload += "\"co2\":" + String(co2Value) + ",";
    payload += "\"humidifier_on\":" + String(isHumidifierOn ? 1 : 0);
    payload += "}";

    Serial.println("Publishing: " + payload);
    client.publish(mqttDataTopic, payload.c_str(), true);
    
    client.loop();
    delay(500); // Buffer for MQTT delivery
  } else {
    Serial.println("\nWiFi Failed - Restarting");
    ESP.restart();
  }

  // --- Lock the relay pin state before sleeping ---
  gpio_hold_en((gpio_num_t)relayPin);
  gpio_deep_sleep_hold_en();

  // Go to sleep
  Serial.println("Entering Deep Sleep...");
  esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIME);
  esp_deep_sleep_start();
}

void loop() {}
