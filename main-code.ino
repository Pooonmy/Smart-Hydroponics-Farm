#include "EEPROM.h"
#include "Arduino.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <math.h>
#include "DHT.h"
#include <UTFT.h>
#include <RTClib.h>
#include "DFRobot_ESP_EC.h"
#include <Adafruit_ADS1X15.h>
#include <DallasTemperature.h>
#include <TridentTD_LineNotify.h>

// Pin Definitions
#define ds18bPin 4
#define dht22Pin 5
#define floatSwitch 6
#define trigPin 10
#define echoPin 11
#define relayPumpA 12
#define relayPumpB 13
#define relayPumpMain 7
#define relayPumpOxygen 15
#define relayGrowLight 16
#define typeSelector 17

// WiFi and MQTT setup
WiFiClient espClient;
PubSubClient client(espClient);

// WiFi credentials and Line Notify token
// const char* ssid = "LAPTOP-POONMY";
// const char* password = "asdfghjkl";
const char* ssid = "WATTANA-2.4G";
const char* password = "21512153";
const char* lineToken = "LEy8r82KcDFDHiKKgNgb8gNR1P2Obwzf2rCYDDmk4N3";
const char* mqtt_broker = "broker.emqx.io";
const int mqtt_port = 1883;

// Sensor and display objects
RTC_DS3231 rtc;
DFRobot_ESP_EC ec;
Adafruit_ADS1115 ads;
DHT dht(dht22Pin, DHT22);
OneWire oneWire(ds18bPin);
DallasTemperature ds18b(&oneWire);
//.........(Model, SDA, SCL, CS, RST, RS)
UTFT myGLCD(ST7735, 40, 39, 38, 42, 41);
extern uint8_t SmallFont[];

// Global Variables for sensor values, status, and configuration
bool switchState;
int vegType = -1;
int ecNeeded = 0;
int ecDebounce = 0;
int tempStatus = 0;
int waterStatus = 0;
int waterLevelInt = 0;
int vegTypeSwitch = 0;
int oldWaterLevel = 0;
int notifyCounter = 0;
int secondNotifyCounter = 0;
float dht22Temp = 0;
float dht22Humid = 0;
float ds18bTemp = 0;
float waterLevel = 0;
float voltage = 0;
float ecValue = 0;
String sensorsDataString = "";
String statusWarningString = "";

// Configurable Settings
int emptyDistance = 46;      // Distance when the tank is empty
int fullDistance = 20;        // Distance when the tank is full
int maxVegType = 2;          // Number of vegetable types
int activatingInterval = 5;  // Time interval in minutes for checking sensors
int nutrientInterval = 0.5;  // Time in minutes for nutrient pump to run

// ===============================================
// Sensor Reading Functions
// ===============================================

void readDs18bSensor() {
  // Read water temperature from DS18B20
  ds18b.requestTemperatures();
  ds18bTemp = ds18b.getTempCByIndex(0);
  Serial.print("Water Temperature : ");
  Serial.print(ds18bTemp);
  Serial.println(" °C");
}

void readDht22Sensor() {
  // Read ambient temperature and humidity from DHT22
  dht22Temp = dht.readTemperature();
  dht22Humid = dht.readHumidity();
  Serial.print("Ambient Temperature : ");
  Serial.print(dht22Temp);
  Serial.println(" °C");
  Serial.print("Ambient Humidity : ");
  Serial.print(dht22Humid);
  Serial.println("%");
}

void readEcSensor() {
  // Read electrical conductivity (EC) value
  voltage = ads.readADC_SingleEnded(0) / 10;
  ecValue = ec.readEC(voltage, ds18bTemp);  // EC value with temperature compensation
  Serial.print("EC:");
  Serial.print(ecValue, 4);
  Serial.println("ms/cm");
  ec.calibration(voltage, ds18bTemp);  // Calibration for EC sensor
}

void waterLevelNotify() {
  // Notify water level at specific percentages
  if (waterLevelInt == 10) LINE.notify("Water level is 10%");
  else if (waterLevelInt == 20) LINE.notify("Water level is 20%");
  else if (waterLevelInt == 30) LINE.notify("Water level is 30%");
  else if (waterLevelInt == 40) LINE.notify("Water level is 40%");
  else if (waterLevelInt == 50) LINE.notify("Water level is 50%");
  else if (waterLevelInt == 60) LINE.notify("Water level is 60%");
  else if (waterLevelInt == 70) LINE.notify("Water level is 70%");
  else if (waterLevelInt == 80) LINE.notify("Water level is 80%");
  else if (waterLevelInt == 90) LINE.notify("Water level is 90%");
  else if (waterLevelInt == 100) LINE.notify("Water level is 100%");
}

void readWaterLevel() {
  // Read water level using ultrasonic sensor
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Measure the duration of sound waves
  long duration = pulseIn(echoPin, HIGH);
  float soundSpeed = (331.5 + (0.6 * dht22Temp)) / 10000;
  float distanceCM = (duration * soundSpeed) / 2;

  // Calculate water level as a percentage
  waterLevel = ((emptyDistance - distanceCM) / (emptyDistance - fullDistance)) * 100;
  waterLevel = roundf(waterLevel * 100) / 100;
  waterLevelInt = waterLevel;

  // Notify at every 10% level change
  if (waterLevelInt % 10 == 0) {
    notifyCounter++;
    if (notifyCounter == 500) {
      waterLevelNotify();
    } else if (notifyCounter >= 10000) {
      notifyCounter = 0;
    }
  } else {
    notifyCounter = 0;
  }

  Serial.print("Distance : ");
  Serial.print(distanceCM);
  Serial.println(" cm");
}

// ===============================================
// MQTT Publish Functions
// ===============================================

void publishSensorsData() {
  // Publish sensor data to MQTT
  sensorsDataString = String(ds18bTemp) + "," + String(dht22Temp) + "," + String(ecValue) + "," + String(waterLevel);
  byte arrSize = sensorsDataString.length() + 1;
  char msg[arrSize];

  Serial.print("Sensors Data : ");
  Serial.println(sensorsDataString);

  sensorsDataString.toCharArray(msg, arrSize);
  client.publish("projectHydro5/sensorsData", msg);
}

void publishStatusWarning() {
  // Publish system status and warnings to MQTT
  int nutrientPumpStatus = digitalRead(relayPumpA);
  int waterPumpStatus = digitalRead(relayPumpMain);
  int oxygenPumpStatus = digitalRead(relayPumpOxygen);
  int growLightStatus = digitalRead(relayGrowLight);

  statusWarningString = String(nutrientPumpStatus) + "," + String(waterPumpStatus) + "," + String(oxygenPumpStatus) + "," + String(growLightStatus) + "," + String(waterStatus) + "," + String(tempStatus);
  byte arrSize = statusWarningString.length() + 1;
  char msg[arrSize];

  Serial.print("Status and Warning : ");
  Serial.println(statusWarningString);

  statusWarningString.toCharArray(msg, arrSize);
  client.publish("projectHydro5/statusWarning", msg);

  // Send Line notifications based on water and temperature status
  if (waterStatus == 1) LINE.notify("Water level is too low, refill required!");
  if (tempStatus == 1) LINE.notify("Temperature is too high, cooling needed!");
}

// ===============================================
// LCD Update Functions
// ===============================================

void lcdVegType() {
  // Display vegetable type and update EC needed based on type
  if (abs(vegType) % maxVegType == 0) {
    ecNeeded = 0.2;
    myGLCD.print(String("Type 1 Salads"), CENTER, 2);
    client.publish("projectHydro5/vegTypeStatus", "0");
  } else if (abs(vegType) % maxVegType == 1) {
    ecNeeded = 0.1;
    myGLCD.print(String("Type 2 Thai Vegs"), CENTER, 2);
    client.publish("projectHydro5/vegTypeStatus", "1");
  }
}

void lcdUpdate() {
  // Set background color to black and text color to light blue
  myGLCD.setBackColor(0, 0, 0);
  myGLCD.setColor(0, 127, 255);

  // Display EC Value and its unit on the LCD
  myGLCD.print(String("EC Value: "), 0, 25);
  myGLCD.print(String(ecValue), 75, 25);
  myGLCD.print(String("mS/cm"), 110, 25);

  // Set text color to green and display the water level
  myGLCD.setColor(0, 255, 0);
  myGLCD.print(String("Water Level: "), 0, 45);

  // Check if water level is between 0 and 99%
  if (waterLevelInt >= 0 && waterLevelInt < 100) {
    // If the water level has changed since last update, clear old value from the screen
    if (oldWaterLevel != waterLevelInt) {
      myGLCD.setBackColor(0, 0, 0);
      myGLCD.print(String("   "), 110, 45);
      myGLCD.print(String("   "), 130, 45);
      myGLCD.print(String("%"), 115, 45);
      oldWaterLevel = waterLevelInt;
    }
    // Display current water level
    myGLCD.print(String(waterLevelInt), 95, 45);
  } else if (waterLevelInt == 100) {
    myGLCD.print(String("100 %      "), 95, 45);
  } else {
    // If water level is out of bounds, display an error message
    myGLCD.print(String("Error      "), 95, 45);
  }

  // Set text color to magenta and display water temperature
  myGLCD.setColor(255, 0, 255);
  myGLCD.print(String("Water Temp: "), 0, 65);
  myGLCD.print(String(ds18bTemp), 90, 65);
  myGLCD.print(String("C"), 135, 65);

  // Set text color to blue and display ambient temperature
  myGLCD.setColor(0, 0, 255);
  myGLCD.print(String("Temperature: "), 0, 85);
  myGLCD.print(String(dht22Temp), 97, 85);
  myGLCD.print(String("C"), 142, 85);

  // Set text color to yellow and display ambient humidity
  myGLCD.setColor(255, 255, 0);
  myGLCD.print(String("Humidity: "), 0, 105);
  myGLCD.print(String(dht22Humid), 75, 105);
  myGLCD.print(String("%"), 120, 105);

  // Check if the vegetable type has changed (vegTypeSwitch > vegType)
  if (vegTypeSwitch > vegType) {
    myGLCD.setColor(255, 0, 0);
    myGLCD.fillRect(0, 0, 160, 15);
    myGLCD.setColor(255, 255, 255);

    // Update the vegetable type and print the new type
    vegType++;
    myGLCD.setBackColor(255, 0, 0);
    lcdVegType();
    Serial.println(vegType);
  }
  myGLCD.setBackColor(0, 0, 0);
}

// ===============================================
// Setup Function
// ===============================================

void setup() {
  // Initialize Serial communication, LCD, WiFi, MQTT, sensors, and set up pins
  Serial.begin(115200);
  myGLCD.InitLCD();  // Initialize the LCD display
  myGLCD.setFont(SmallFont);
  myGLCD.clrScr();                      // Clear the LCD screen
  setup_wifi();                         // Set up WiFi connection
  reconnect();                          // Connect to the MQTT broker
  LINE.setToken(lineToken);             // Set Line messaging token
  client.subscribe("projectHydro5/#");  // Subscribe to MQTT topic

  // Initialize EEPROM, sensors, and RTC (Real Time Clock)
  EEPROM.begin(32);
  ec.begin();                                      // Begin EC sensor
  ads.setGain(GAIN_ONE);                           // Set ADC gain for the ADS sensor
  ads.begin();                                     // Begin ADS sensor
  dht.begin();                                     // Begin DHT22 sensor
  ds18b.begin();                                   // Begin DS18B20 temperature sensor
  rtc.begin();                                     // Begin RTC
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));  // Adjust RTC to compile date/time

  // Set initial colors and display the vegetable type
  myGLCD.setColor(255, 255, 255);
  myGLCD.setBackColor(255, 0, 0);
  lcdVegType();  // Display vegetable type on the LCD

  // Configure pin modes for sensors, relays, and float switch
  pinMode(trigPin, OUTPUT);  // Water level sensor (ultrasonic)
  pinMode(echoPin, INPUT);
  pinMode(relayPumpA, OUTPUT);          // Nutrient pump A
  pinMode(relayPumpB, OUTPUT);          // Nutrient pump B
  pinMode(relayPumpMain, OUTPUT);       // Main water pump
  pinMode(relayPumpOxygen, OUTPUT);     // Oxygen pump
  pinMode(relayGrowLight, OUTPUT);      // Grow light
  pinMode(floatSwitch, INPUT_PULLUP);   // Water level float switch
  pinMode(typeSelector, INPUT_PULLUP);  // Veg type selector button

  // Set all relays to off state (HIGH = off)
  digitalWrite(relayPumpA, HIGH);
  digitalWrite(relayPumpB, HIGH);
  digitalWrite(relayPumpMain, HIGH);
  digitalWrite(relayPumpOxygen, HIGH);
  digitalWrite(relayGrowLight, HIGH);

  // Publish initial vegetable type status to MQTT
  client.publish("projectHydro5/vegTypeStatus", "0");
}

// ===============================================
// Main Loop
// ===============================================

void loop() {
  // Main loop to manage sensors, pumps, lights, and publish data
  static unsigned long timepoint = millis();  // Store the time point
  readDht22Sensor();                          // Read temperature and humidity from DHT22
  readDs18bSensor();                          // Read temperature from DS18B20
  readWaterLevel();                           // Read water level from ultrasonic sensor
  readEcSensor();                             // Read electrical conductivity (EC) sensor

  // Manage nutrient and oxygen pumps based on EC value
  if (ecValue <= ecNeeded && ecDebounce <= activatingInterval * 60) {
    // EC is low, but not time for pump activation yet
    ecDebounce++;
    deactivatePumpA();
    deactivatePumpB();
    deactivateOxygenPump();
  } else if (ecValue <= ecNeeded && ecDebounce >= activatingInterval * 60) {
    // EC is low and it's time to activate pumps
    activatePumpA();
    activatePumpB();
    activateOxygenPump();
    if (ecDebounce >= (activatingInterval + nutrientInterval) * 60) {
      // Reset debounce counter after nutrient and oxygen cycle
      ecDebounce = 0;
    }
    ecDebounce++;
  } else {
    // EC is sufficient, deactivate the pumps
    deactivatePumpA();
    deactivatePumpB();
    deactivateOxygenPump();
  }

  // Manage water pump based on water level and float switch
  switchState = digitalRead(floatSwitch);  // Read the float switch state
  Serial.println(switchState);
  if (waterLevel <= 30 || switchState == 0) {
    waterStatus = -1;  // Water is low or float switch is triggered, turn off main pump
    deactivateMainPump();
  } else if (waterLevel >= 95) {
    waterStatus = 1;  // Water is full
    activateMainPump();
  } else {
    waterStatus = 0;  // Water is within a normal range
    activateMainPump();
  }

  // Manage temperature status (overheat or too cold)
  if (dht22Temp >= 40 || ds18bTemp >= 40) {
    tempStatus = 1;  // Temperature too high
  } else if (dht22Temp <= 15 || ds18bTemp <= 15) {
    tempStatus = -1;  // Temperature too low
  } else {
    tempStatus = 0;  // Temperature is within normal range
  }

  // Control grow light based on the current time
  DateTime now = rtc.now();
  Serial.print(now.hour(), DEC);  // Print the current hour
  if (now.hour() >= 6 && now.hour() <= 20) {
    activateGrowLight();  // Daytime: turn on grow light
  } else {
    deactivateGrowLight();  // Nighttime: turn off grow light
  }

  // Detect change in vegetable type
  if (digitalRead(typeSelector) == LOW) {
    myGLCD.clrScr();    // Clear screen on button press
    vegTypeSwitch++;    // Increment the vegType switch count
    oldWaterLevel = 0;  // Reset the water level display
    while (digitalRead(typeSelector) == LOW) {
      delay(10);  // Debounce the button press
    }
  }

  // Publish sensor data and status updates
  publishSensorsData();
  publishStatusWarning();
  lcdUpdate();  // Update the LCD display with new data
  delay(1000);  // Delay for 1 second
}

// Function to handle WiFi setup and connection
void setup_wifi() {
  myGLCD.clrScr();
  myGLCD.setColor(255, 255, 255);
  myGLCD.setBackColor(0, 0, 0);
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);  // Set WiFi to station mode
  WiFi.begin(ssid, password);

  // Keep printing "Connecting to WiFi..." until connection is established
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to WiFi...");
    myGLCD.print(String("Connecting to WiFi."), CENTER, 48);
    delay(300);
    myGLCD.print(String("Connecting to WiFi.."), CENTER, 48);
    delay(300);
    myGLCD.print(String("Connecting to WiFi..."), CENTER, 48);
    delay(300);
    myGLCD.print(String("Connecting to WiFi."), CENTER, 48);
    delay(300);
    myGLCD.print(String("Connecting to WiFi.."), CENTER, 48);
    delay(300);
    myGLCD.print(String("Connecting to WiFi..."), CENTER, 48);
    delay(300);
  }

  // Once connected, print success messages
  Serial.println("WiFi connected");
  myGLCD.print(String("WiFi connected"), CENTER, 68);
  delay(1000);
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());  // Print the local IP address
  myGLCD.clrScr();
}

// MQTT reconnection function
void reconnect() {
  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(callback);  // Set the MQTT callback function
  while (!client.connected()) {
    String client_id = "esp32-client-";  // Generate unique client ID
    client_id += String(WiFi.macAddress());
    Serial.printf("The client %s connects to the public mqtt broker\n", client_id.c_str());
    if (client.connect(client_id.c_str()))  // Try to connect to the MQTT broker
      Serial.println("Public emqx mqtt broker connected");
    else {
      Serial.print("failed with state ");
      Serial.print(client.state());  // Print connection error state
      delay(2000);                   // Retry after 2 seconds
    }
  }
}

// MQTT message callback
void callback(char* topic, byte* payload, unsigned int length) {
  payload[length] = '\0';  // Null-terminate the payload
  if (strcmp(topic, "projectHydro5/button") == 0) {
    Serial.println((char*)payload);  // Print the payload received
    if (strcmp((char*)payload, "true") == 0) {
      Serial.println("hello");  // Print "hello" if payload is "true"
    }
  }
}

// Activate and deactivate pump A
void activatePumpA() {
  digitalWrite(relayPumpA, LOW);
  Serial.println("Pump A activated");
}

void deactivatePumpA() {
  digitalWrite(relayPumpA, HIGH);
  Serial.println("Pump A deactivated");
}

// Activate and deactivate pump B
void activatePumpB() {
  digitalWrite(relayPumpB, LOW);
  Serial.println("Pump B activated");
}

void deactivatePumpB() {
  digitalWrite(relayPumpB, HIGH);
  Serial.println("Pump B deactivated");
}

// Activate and deactivate the main pump
void activateMainPump() {
  digitalWrite(relayPumpMain, LOW);
  Serial.println("Pump main activated");
}

void deactivateMainPump() {
  digitalWrite(relayPumpMain, HIGH);
  Serial.println("Pump main deactivated");
}

// Activate and deactivate oxygen pump
void activateOxygenPump() {
  digitalWrite(relayPumpOxygen, LOW);
  Serial.println("Pump oxygen activated");
}

void deactivateOxygenPump() {
  digitalWrite(relayPumpOxygen, HIGH);
  Serial.println("Pump oxygen deactivated");
}

// Activate and deactivate grow light
void activateGrowLight() {
  digitalWrite(relayGrowLight, LOW);
  Serial.println("Grow light activated");
}

void deactivateGrowLight() {
  digitalWrite(relayGrowLight, HIGH);
  Serial.println("Grow light deactivated");
}
