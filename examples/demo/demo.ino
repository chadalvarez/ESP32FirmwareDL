// 2025Feb11 CHAD
// WORKING VVVVVVG

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <ESPAsyncWebServer.h>
#include "ESP32FirmwareDownloader.h"

// WiFi credentials
const char* ssid = "ssid";
const char* password = "pw";

// SD Card SPI pins
#define SD_CS    5
#define SD_MOSI  17
#define SD_MISO  16
#define SD_CLK   4

// Create an AsyncWebServer on port 80.
AsyncWebServer server(80);

// Create an instance of the firmware downloader library.
// Default endpoint is "/dumpflash" and default filename is "fullclone.bin".
ESP32FirmwareDownloader firmwareDownloader("/dumpflash", "fullclone.bin");

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("[setup] Starting setup...");

  // Connect to WiFi.
  Serial.println("[setup] Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[setup] Connected. IP address: ");
  Serial.println(WiFi.localIP());

  // Initialize SPI bus and SD card.
  Serial.println("[setup] Initializing SD card...");
  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("[setup] SD card initialization failed!");
  } else {
    Serial.println("[setup] SD card initialized successfully.");
  }

  // Optionally, set a custom filename.
  // firmwareDownloader.setFilename("myfirmware.bin");

  // Attach the firmware download feature to the server.
  // The second parameter 'true' tells the library to auto-erase the user data partition.
  if (!firmwareDownloader.attach(server, true)) {
    Serial.println("[setup] Firmware downloader attachment failed (SD not available?)");
  }

  // Attach OTA partition endpoints.
  if (!firmwareDownloader.attachOTA(server)) {
    Serial.println("OTA downloader attachment failed (SD not available?)");
  }


  // Register a root route.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html",
      "<html><head><title>ESP32 Firmware Download Demo</title></head><body>"
      "<h1>Firmware Download Demo</h1>"
      "<p><a href=\"/dumpflash\">Download full flash clone</a></p>"
      "<p><a href=\"/downloadota0\">Download OTA0 partition</a></p>"
      "<p><a href=\"/downloadota1\">Download OTA1 partition</a></p>"
      "</body></html>");
  });

  server.begin();
  Serial.println("[setup] HTTP server started.");
}

void loop() {
}
