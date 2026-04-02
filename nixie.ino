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
const uint32_t ota_enable_pin = 19;

// NTP
const char *ntpServer1 = "pool.ntp.org";
const char *ntpServer2 = "time.nist.gov";
//const long gmtOffset_sec = 3600;
//const int daylightOffset_sec = 3600;
const char *time_zone = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Budapest with daylight savings
struct tm current_time;

// Timer
hw_timer_t *timer = NULL;
volatile SemaphoreHandle_t update_required;
uint32_t update_interval = 200000;

// WebSerial
AsyncWebServer server(80);

// Digit selector shift register
const uint32_t sr_serial_pin = 26;// Serial data pin 
const uint32_t sr_clock_pin = 33; // Clock pin
const uint32_t sr_clear_pin = 32; // Clear pin (inverted)
const uint32_t sr_latch_pin = 25; // Latch pin
uint32_t sr_currrent_digit = 100; // Keep track of current digit for optimization


// Set up shift register
void sr_setup() {
  pinMode(sr_serial_pin, OUTPUT);
  pinMode(sr_clock_pin, OUTPUT);
  pinMode(sr_clear_pin, OUTPUT);
  pinMode(sr_latch_pin, OUTPUT);

  digitalWrite(sr_serial_pin, false);
  digitalWrite(sr_clock_pin, false);
  digitalWrite(sr_clear_pin, true);
  digitalWrite(sr_latch_pin, false);

  sr_clear();
  sr_latch();
}

// Latch shift register (write internal state to outputs all at once)
inline void sr_latch() {
  digitalWrite(sr_latch_pin, true);
  //delayMicroseconds(1);
  digitalWrite(sr_latch_pin, false);
  //delayMicroseconds(1);
}

// Clear shift register (set all internal states to false)
inline void sr_clear() {
  digitalWrite(sr_clear_pin, false);
  //delayMicroseconds(1);
  digitalWrite(sr_clear_pin, true);
  //delayMicroseconds(1);
}

// Push a bit to the internal shift register
inline void sr_pulse_clock(uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    digitalWrite(sr_clock_pin, true);
    //delayMicroseconds(1);
    digitalWrite(sr_clock_pin, false);
    //delayMicroseconds(1);
  }
}

inline void sr_set_serial(bool data) {
  digitalWrite(sr_serial_pin, data);
  //delayMicroseconds(1);
}

inline void sr_push(bool data) {
  digitalWrite(sr_serial_pin, data);
  digitalWrite(sr_clock_pin, true);
  digitalWrite(sr_clock_pin, false);
}

// Set a digit on the shift register
// Takes about 18 us to reset to 0, then count up to 9 (~50 kHz)
void sr_set_digit(uint32_t digit) {
  if (digit >= sr_currrent_digit) {
    // Send out sr_currrent_digit - digit zero bits
    sr_set_serial(false);
    sr_pulse_clock(digit - sr_currrent_digit);
  }
  else {
    // Set all bits to zero
    sr_clear();

    // Send out the true output to the zero digit
    sr_set_serial(true);
    sr_pulse_clock(1);

    // Push it to the desired place
    sr_set_serial(false);
    sr_pulse_clock(digit);
  }

  // Store current digit
  sr_currrent_digit = digit;
  // Output the shift register's contents
  sr_latch();
}

// Sets the shift register to display the given digit on the given tube
// Takes around 30 us to finish, the previous state is retained on the outputs during this time
void sr_set_state(uint32_t tube, uint32_t digit) {
  // handle overflows
  if (tube > 5 || digit > 9)
    return;
  
  uint32_t state = 0;
  state += 1 << 5 - tube;
  state += 1 << 15 - digit;
  
  sr_clear();
  for (uint32_t i = 0; i < 16; ++i) {
    sr_push(state & 0x0001);
    state >>= 1;
  }
  sr_latch();
}

// Get the current time
// Takes approximately 40 us to return (if successful)
struct tm getTime() {
  struct tm timeinfo;
  // Spin until time is available
  while(!getLocalTime(&timeinfo)) {
    WebSerial.println("No time available, retrying in 500 ms.");
    delay(500);
  }
  return timeinfo;
}

// NTP time adjustment callback
void timeavailable(struct timeval *t) {
  WebSerial.println("Got time adjustment from NTP!");
}

void ARDUINO_ISR_ATTR timerInterrupt() {
  xSemaphoreGiveFromISR(update_required, NULL);
}

// Set up NPT and timer
void setupClock() {
  // Configure NTP-backed clock
  sntp_set_time_sync_notification_cb(timeavailable); // Set callback on NTP update
  configTzTime(time_zone, ntpServer1, ntpServer2);   // Set up NTP and timezones

  // Configure timer interrupts
  timer = timerBegin(1000000);                       // 1 us resolution
  timerAttachInterrupt(timer, &timerInterrupt);      // Attach callback
  timerAlarm(timer, update_interval, true, 0);       // Set up alarms
  update_required = xSemaphoreCreateBinary();        // Initialize semaphore

  current_time = getTime();                          // Initialize clock 
}

// Set up OTA
void setupOTA() {
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

  setupOTA();

  setupClock();

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

  /*for (uint32_t i = 0; i < 10; ++i) {
    Serial.println(sr_set_state(0, i));
    delay(100);
  }
  Serial.println("");
  delay(2000);*/
  
  // Update the time variable if the timer asks for it
  if (xSemaphoreTake(update_required, 0) == pdTRUE) {
    current_time = getTime();
    WebSerial.println(current_time.tm_sec);
    sr_set_state(0, current_time.tm_sec % 10);
  }
}
