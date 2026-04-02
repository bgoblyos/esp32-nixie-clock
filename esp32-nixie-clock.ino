// Copyright 2026 Bence Göblyös

#include <WiFi.h>
#include <ESPmDNS.h>
#include <NetworkUdp.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <esp_sntp.h>

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WString.h>
#include <WebSerialLite.h>
#include <AsyncTCP.h>

// Secrets
#include "secrets.h"

// WiFi and OTA
// "const char *ssid" from secrets.h
// "const char *password" from secrets.h
// "const char *ota_password" from secrets.h
uint32_t last_ota_time = 0;
const uint32_t ota_port = 3232;
const char *ota_hostname = "esp32-nixie";
const uint32_t ota_enable_pin = 19;       // OTA is only enabled when this pin is high

// NTP
const char *ntp_server_1 = "pool.ntp.org";
const char *ntp_server_2 = "pool.chrony.eu";
const char *time_zone = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Budapest with daylight savings
struct tm current_time;

// Timer
hw_timer_t *timer = NULL;                   // Timer to trigger clock updates
volatile SemaphoreHandle_t update_required; // Semaphore to indicate whether the clock should update the displayed time
const uint32_t update_interval = 200000;    // Update every 200 ms

// WebSerial
AsyncWebServer server(80);

// Shift register for tube and digit selection
const uint32_t sr_serial_pin = 26; // Serial data pin 
const uint32_t sr_clock_pin = 33;  // Clock pin
const uint32_t sr_clear_pin = 32;  // Clear pin (inverted)
const uint32_t sr_latch_pin = 25;  // Latch pin

// Set up shift register
void sr_setup() {
  // Set directions
  pinMode(sr_serial_pin, OUTPUT);
  pinMode(sr_clock_pin, OUTPUT);
  pinMode(sr_clear_pin, OUTPUT);
  pinMode(sr_latch_pin, OUTPUT);

  // Set to idle
  digitalWrite(sr_serial_pin, false);
  digitalWrite(sr_clock_pin, false);
  digitalWrite(sr_clear_pin, true);
  digitalWrite(sr_latch_pin, false);

  // Set all outputs to 0
  sr_clear();
  sr_latch();
}

// Latch shift register (write internal state to outputs all at once)
inline void sr_latch() {
  digitalWrite(sr_latch_pin, true);
  digitalWrite(sr_latch_pin, false);
}

// Clear shift register (set all internal states to 0)
inline void sr_clear() {
  digitalWrite(sr_clear_pin, false);
  digitalWrite(sr_clear_pin, true);
}

// Push bit to shift register
inline void sr_push(bool data) {
  digitalWrite(sr_serial_pin, data);
  digitalWrite(sr_clock_pin, true);
  digitalWrite(sr_clock_pin, false);
}

// Sets the shift register to display the given digit on the given tube
// Takes around 30 us to finish, the previous state is retained on the outputs during this time
void sr_set_state(uint32_t tube, uint32_t digit) {
  // Handle out of bounds errors
  if (tube > 5 || digit > 9)
    return;
  
  // Encode state bitstring
  uint32_t state = 0;
  state += 1 << (5 - tube);   
  state += 1 << (15 - digit);
  // Example: 0b1000000000100000 lights up the digit 0 on tube 0
  // Mapping:   0123456789abcdef, 0-9 digits, a-f tubes (indexed 0 to 5)
  
  // Clear shift register and send bitsring
  sr_clear();
  for (uint32_t i = 0; i < 16; ++i) {
    sr_push(state & 0x0001);  // Push the last bit in the state to the shift register
    state >>= 1;              // Discard last bit and queue up the next one
  }

  // Output the newly transfered state
  sr_latch();
}

// Get the current time
// Takes approximately 40 us to return (if successful)
struct tm get_time() {
  struct tm timeinfo;
  // Spin until time is available
  while(!getLocalTime(&timeinfo)) {
    WebSerial.println("No time available, retrying in 500 ms.");
    delay(500);
  }
  return timeinfo;
}

void ARDUINO_ISR_ATTR timer_interrupt() {
  xSemaphoreGiveFromISR(update_required, NULL);
}

// Set up NPT and timer
void clock_setup() {
  // Configure NTP-backed clock
  configTzTime(time_zone, ntp_server_1, ntp_server_2); // Set up NTP and timezones
  current_time = get_time();                           // Initialize time variable

  // Configure timer interrupts
  update_required = xSemaphoreCreateBinary();          // Initialize semaphore
  timer = timerBegin(1000000);                         // 1 us resolution
  timerAttachInterrupt(timer, &timer_interrupt);       // Attach callback
  timerAlarm(timer, update_interval, true, 0);         // Set up alarms
}

// Set up OTA
void OTA_setup() {
  pinMode(ota_enable_pin, INPUT);

  ArduinoOTA.setPort(ota_port);
  ArduinoOTA.setHostname(ota_hostname);
  ArduinoOTA.setPassword(ota_password);

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else {  // U_SPIFFS
        type = "filesystem";
      }

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      if (millis() - last_ota_time > 500) {
        Serial.printf("Progress: %u%%\n", (progress / (total / 100)));
        last_ota_time = millis();
      }
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        Serial.println("Auth Failed");
      } else if (error == OTA_BEGIN_ERROR) {
        Serial.println("Begin Failed");
      } else if (error == OTA_CONNECT_ERROR) {
        Serial.println("Connect Failed");
      } else if (error == OTA_RECEIVE_ERROR) {
        Serial.println("Receive Failed");
      } else if (error == OTA_END_ERROR) {
        Serial.println("End Failed");
      }
    });

  ArduinoOTA.begin();
}

void setup() {
  Serial.begin(115200);
  Serial.println("Booting...");

  // Initilaize WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  OTA_setup();
  clock_setup();
  sr_setup();
  
  // Set up WebSerial
  WebSerial.begin(&server);
  server.onNotFound([](AsyncWebServerRequest* request) { request->redirect("/webserial"); });
  server.begin();

  Serial.println("System startup complete.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // If the OTAenable pin is high, run the handler and reset the loop
  if (digitalRead(ota_enable_pin)) {
    ArduinoOTA.handle();
    return;
  }
  
  // Update the time variable if the timer asks for it
  if (xSemaphoreTake(update_required, 0) == pdTRUE) {
    current_time = get_time();
    WebSerial.println(current_time.tm_sec);
    sr_set_state(0, current_time.tm_sec % 10);
  }
}
