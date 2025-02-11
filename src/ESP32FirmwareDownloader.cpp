#include "ESP32FirmwareDownloader.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include "esp_flash.h"         // For esp_flash_read() and esp_flash_default_chip
#include "esp_partition.h"     // For partition APIs
#include "esp_image_format.h"  // For esp_image_header_t (not used in the full dump)
#include <esp_task_wdt.h>      // For esp_task_wdt_reset()
#include <esp_err.h>

#ifndef ESP_IMAGE_HEADER_MAGIC
#define ESP_IMAGE_HEADER_MAGIC 0xE9
#endif

// Initialize the static instance pointer.
ESP32FirmwareDownloader* ESP32FirmwareDownloader::_instance = nullptr;

ESP32FirmwareDownloader::ESP32FirmwareDownloader(const char* endpoint, const String &filename)
  : _endpoint(endpoint),
    _firmwareFilename(filename),
    _blankOffset(0),
    _blankLength(0)
{
  // Assume a single instance.
  _instance = this;
}

bool ESP32FirmwareDownloader::attach(AsyncWebServer &server, bool eraseUserData) {
  // Check if the SD card is available.
  if (SD.cardType() == CARD_NONE) {
    Serial.println("[ESP32FirmwareDownloader] SD card is not available!");
    return false;
  }
  // If eraseUserData is true, attempt to auto-detect the user data partition.
  if (eraseUserData) {
    if (autoSetUserDataBlank()) {
      Serial.println("[ESP32FirmwareDownloader] User data partition auto-detected and will be blanked.");
    } else {
      Serial.println("[ESP32FirmwareDownloader] Could not auto-detect user data partition. No blanking will occur.");
    }
  }
  // Attach the HTTP GET handler at the specified endpoint.
  server.on(_endpoint, HTTP_GET, handleDumpFlash);
  return true;
}

void ESP32FirmwareDownloader::setFilename(const String &filename) {
  _firmwareFilename = filename;
}

void ESP32FirmwareDownloader::setBlankRegion(uint32_t offset, uint32_t length) {
  _blankOffset = offset;
  _blankLength = length;
}

bool ESP32FirmwareDownloader::autoSetUserDataBlank() {
  // Attempt to find a data partition with the label "userdata"
  const esp_partition_t* userPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "userdata");
  if (userPart != NULL) {
    Serial.printf("[ESP32FirmwareDownloader] Found user data partition '%s' at 0x%08X, size: %u bytes\n",
                  userPart->label, userPart->address, userPart->size);
    setBlankRegion(userPart->address, userPart->size);
    // DO NOT call esp_partition_iterator_release() here because userPart is a partition pointer, not an iterator.
    return true;
  } else {
    Serial.println("[ESP32FirmwareDownloader] No user data partition found with label 'userdata'.");
    return false;
  }
}

void ESP32FirmwareDownloader::handleDumpFlash(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Dump flash request received.");
  
  // Get the total flash size.
  uint32_t flashSize = ESP.getFlashChipSize();
  Serial.printf("[ESP32FirmwareDownloader] Flash size: %u bytes\n", flashSize);

  // Use the stored firmware filename.
  String fname = "fullclone.bin";
  if (_instance) {
    fname = _instance->_firmwareFilename;
  }
  String filename = "/" + fname;
  
  if (SD.exists(filename)) {
    Serial.printf("[ESP32FirmwareDownloader] File %s exists. Removing it...\n", filename.c_str());
    SD.remove(filename);
  }
  
  // Open the file for writing.
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("[ESP32FirmwareDownloader] Failed to open file on SD card for writing.");
    request->send(500, "text/plain", "Failed to open file on SD card");
    return;
  }
  
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];
  uint32_t offset = 0;
  uint32_t chunkCounter = 0;
  
  Serial.println("[ESP32FirmwareDownloader] Starting flash dump...");
  while (offset < flashSize) {
    uint32_t toRead = ((flashSize - offset) < chunkSize) ? (flashSize - offset) : chunkSize;
    esp_err_t err = esp_flash_read(esp_flash_default_chip, buffer, offset, toRead);
    if (err != ESP_OK) {
      Serial.printf("[ESP32FirmwareDownloader] Error reading flash at offset %u, error: %d\n", offset, err);
      file.close();
      request->send(500, "text/plain", "Error reading flash data");
      return;
    }
    
    // If a blank region is set, replace bytes in that region with 0xFF.
    if (_instance && _instance->_blankLength > 0) {
      uint32_t chunkStart = offset;
      for (uint32_t i = 0; i < toRead; i++) {
        uint32_t absOffset = chunkStart + i;
        if (absOffset >= _instance->_blankOffset && absOffset < (_instance->_blankOffset + _instance->_blankLength)) {
          buffer[i] = 0xFF;
        }
      }
    }
    
    file.write(buffer, toRead);
    offset += toRead;
    chunkCounter++;
    
    yield();
    delay(5);
    esp_task_wdt_reset();
    
    if (chunkCounter % 10 == 0 || offset >= flashSize) {
      Serial.printf("[ESP32FirmwareDownloader] Dumped %u/%u bytes...\n", offset, flashSize);
    }
  }
  file.close();
  Serial.println("[ESP32FirmwareDownloader] Flash dump completed and written to SD card.");
  
  AsyncWebServerResponse* response = request->beginResponse(SD, filename, "application/octet-stream");
  response->addHeader("Content-Disposition", "attachment; filename=" + fname);
  Serial.printf("[ESP32FirmwareDownloader] Serving file %s for download.\n", filename.c_str());
  request->send(response);
}
