#ifndef ESP32FIRMWAREDOWNLOADER_H
#define ESP32FIRMWAREDOWNLOADER_H
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

class ESP32FirmwareDownloader {
public:
  // Constructor: optionally specify the endpoint for the full flash dump
  // (default: "/dumpflash") and the default firmware filename (default: "fullclone.bin").
  ESP32FirmwareDownloader(const char* endpoint = "/dumpflash", const String &filename = "fullclone.bin");

  // Attach endpoints.
  bool attach(AsyncWebServer &server, bool eraseUserData = false);
  bool attachAll(AsyncWebServer &server, bool eraseUserData = false);

  // Set a custom firmware filename.
  void setFilename(const String &filename);

  // Set a blank region manually.
  void setBlankRegion(uint32_t offset, uint32_t length);

  // Auto-detect a single or multiple user-data partitions to blank.
  bool autoSetUserDataBlank();
  bool autoSetUserDataBlankAll();

private:
  const char* _endpoint;
  String _firmwareFilename;
  uint32_t _blankOffset;
  uint32_t _blankLength;

  // Support for multiple blank regions.
  static const int MAX_BLANK_REGIONS = 4;
  struct BlankRegion {
    uint32_t offset;
    uint32_t length;
    const char* description;
  };
  static BlankRegion _blankRegions[MAX_BLANK_REGIONS];
  static int _numBlankRegions;

  // Single-instance pointer.
  static ESP32FirmwareDownloader* _instance;

  // Callback functions for streaming.
  static size_t flashStreamCallback(uint8_t *buffer, size_t maxLen, size_t index);
  static size_t flashStreamCallbackBlanked(uint8_t *buffer, size_t maxLen, size_t index);
  static size_t genericPartStreamCallback(uint8_t *buffer, size_t maxLen, size_t index);

  // HTTP endpoint handlers.
  static void handleDumpFlash(AsyncWebServerRequest *request);
  static void handleDumpFlashSecure(AsyncWebServerRequest *request);
  static void handleDownloadPartitionDirect(AsyncWebServerRequest *request);
  static void handleDownloadBoot(AsyncWebServerRequest *request);
  static void handleActivatePartition(AsyncWebServerRequest *request);
  static void handleClonePartition(AsyncWebServerRequest *request);
  static void handleListPartitions(AsyncWebServerRequest *request);
  static void handleRoot(AsyncWebServerRequest *request);
  static void handleHexDump(AsyncWebServerRequest *request);
  static void handleUploadBinary(AsyncWebServerRequest *request,
                                 const String &filename,
                                 size_t index,
                                 uint8_t *data,
                                 size_t len,
                                 bool final);

  // Helper: Add a blank region.
  static void addBlankRegion(uint32_t offset, uint32_t length, const char* description);
};

#endif  // ESP32FIRMWAREDOWNLOADER_H
