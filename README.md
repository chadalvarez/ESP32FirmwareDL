# ESP32FirmwareDownloader

ESP32FirmwareDownloader is an Arduino library for ESP32 that adds a firmware download feature to an AsyncWebServer. The library dumps the entire flash memory to an SD card file (with an option to blank out a specified user data partition) and then serves that file for download over HTTP.

## Features

- **Easy Integration:**  
  Attach the firmware download endpoint to your existing AsyncWebServer instance.

- **User Data Erasure:**  
  Optionally auto-detect a user data partition (by label "userdata") and blank it out (fill with 0xFF) in the downloaded image for security purposes.

- **Custom Download Filename:**  
  Set a custom filename for the firmware download.

## Installation

1. Download or clone this repository.
2. Place the folder `ESP32FirmwareDownloader` in your Arduino `libraries` directory.
3. Restart the Arduino IDE.

## Usage Example

See the `examples/FirmwareDownloadDemo/FirmwareDownloadDemo.ino` file for an example of how to use the library.

### Example Code

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <ESPAsyncWebServer.h>
#include "ESP32FirmwareDownloader.h"

// WiFi credentials
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

// SD Card SPI pins
#define SD_CS    5
#define SD_MOSI  17
#define SD_MISO  16
#define SD_CLK   4

AsyncWebServer server(80);

// Create an instance of the firmware downloader.
// Default endpoint is "/dumpflash" and default filename is "fullclone.bin".
ESP32FirmwareDownloader firmwareDownloader("/dumpflash", "fullclone.bin");

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting setup...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP address: ");
  Serial.println(WiFi.localIP());

  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("SD card initialization failed!");
  } else {
    Serial.println("SD card initialized successfully.");
  }

  // Optionally, set a custom filename.
  // firmwareDownloader.setFilename("myfirmware.bin");

  // Attach the firmware download feature.
  // Pass 'true' to auto-blank the user data partition (if available and labeled "userdata").
  if (!firmwareDownloader.attach(server, true)) {
    Serial.println("Firmware downloader attachment failed (SD not available?)");
  }

  // Register a root route.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", "<html><head><title>Firmware Download Demo</title></head><body><h1>Firmware Download Demo</h1><p>Click <a href=\"/dumpflash\">here</a> to download the firmware image.</p></body></html>");
  });

  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  // Nothing needed here.
}
