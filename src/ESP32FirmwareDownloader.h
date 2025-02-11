#ifndef ESP32FIRMWAREDOWNLOADER_H
#define ESP32FIRMWAREDOWNLOADER_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

class ESP32FirmwareDownloader {
public:
  // Constructor: optionally specify the endpoint (default: "/dumpflash")
  // and the default firmware filename (default: "fullclone.bin").
  ESP32FirmwareDownloader(const char* endpoint = "/dumpflash", const String &filename = "fullclone.bin");

  // Attach the full-flash dump download route to an already-created AsyncWebServer instance.
  // If eraseUserData is true, the library will auto-detect a partition labeled "userdata"
  // and blank it out in the dumped image.
  // Returns true if the SD card is available and the route was attached successfully.
  bool attach(AsyncWebServer &server, bool eraseUserData = false);

  // Attach the OTA partition download routes (OTA0 and OTA1) to the server.
  // Returns true if the SD card is available and the routes were attached successfully.
  bool attachOTA(AsyncWebServer &server);

  // Set a custom firmware filename for the full flash dump (without a leading slash, e.g., "myfirmware.bin").
  void setFilename(const String &filename);

  // Manually set a blank region for the full flash dump (offset and length). Use 0,0 to disable.
  void setBlankRegion(uint32_t offset, uint32_t length);

  // Automatically detect a user data partition by label (e.g., "userdata")
  // and set the blank region accordingly. Returns true if found.
  bool autoSetUserDataBlank();

private:
  const char* _endpoint;
  String _firmwareFilename;
  uint32_t _blankOffset;
  uint32_t _blankLength;

  // Static instance pointer for use in static request handlers (assumes a single instance).
  static ESP32FirmwareDownloader* _instance;

  // Static handler for the full flash dump.
  static void handleDumpFlash(AsyncWebServerRequest *request);

  // Static handlers for OTA partition downloads.
  static void handleDownloadOTA0(AsyncWebServerRequest *request);
  static void handleDownloadOTA1(AsyncWebServerRequest *request);
};

#endif  // ESP32FIRMWAREDOWNLOADER_H
