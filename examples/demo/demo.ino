
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "ESP32FirmwareDownloader.h"

const char* ssid = "ssid"; const char* password = "pw";

AsyncWebServer server(80);
ESP32FirmwareDownloader firmwareDownloader("/dumpflash", "fullclone.bin");

void setup() {
  delay(1000);
  Serial.begin(115200);
  
  // Connect to WiFi.
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println(WiFi.localIP());

  if (!firmwareDownloader.attachAll(server, true)) {
     Serial.println("Firmware downloader attachment failed.");
  }

  // for demo - show the firmware downloader when root is accessed..
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
     request->redirect("/FWDL"); 
  });
  
  server.onNotFound([](AsyncWebServerRequest *request) { request->send(404); });
  server.begin();

  Serial.println("READY.");
}

void loop() {
}