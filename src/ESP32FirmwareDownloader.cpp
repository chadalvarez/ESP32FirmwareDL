#include "ESP32FirmwareDownloader.h"
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include "esp_flash.h"         // For esp_flash_read() and esp_flash_default_chip
#include "esp_partition.h"     // For partition APIs
#include "esp_image_format.h"  // For esp_image_header_t (not used in full flash dump)
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
  // Attach the HTTP GET handler for the full flash dump.
  server.on(_endpoint, HTTP_GET, handleDumpFlash);
  return true;
}

bool ESP32FirmwareDownloader::attachOTA(AsyncWebServer &server) {
  // Check if the SD card is available.
  if (SD.cardType() == CARD_NONE) {
    Serial.println("[ESP32FirmwareDownloader] SD card is not available for OTA download!");
    return false;
  }
  // Attach endpoints for OTA0 and OTA1.
  server.on("/downloadota0", HTTP_GET, handleDownloadOTA0);
  server.on("/downloadota1", HTTP_GET, handleDownloadOTA1);
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
    // Do not call esp_partition_iterator_release() because userPart is a partition pointer.
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

static const size_t OTAChunkSize = 4096;  // chunk size for OTA dump

void ESP32FirmwareDownloader::handleDownloadOTA0(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Download OTA0 request received.");
  // Find OTA_0 partition.
  const esp_partition_t* ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (ota0 == NULL) {
    Serial.println("[ESP32FirmwareDownloader] OTA0 partition not found!");
    request->send(404, "text/plain", "OTA0 partition not found");
    return;
  }
  uint32_t partSize = ota0->size;
  Serial.printf("[ESP32FirmwareDownloader] OTA0 partition found: label=%s, size=%u bytes\n", ota0->label, partSize);
  
  String filename = "/ota0.bin";
  if (SD.exists(filename)) {
    SD.remove(filename);
  }
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    request->send(500, "text/plain", "Failed to open OTA0 file on SD card");
    return;
  }
  
  uint32_t offset = ota0->address; // absolute flash address
  uint32_t endOffset = ota0->address + partSize;
  uint8_t buffer[OTAChunkSize];
  uint32_t chunkCounter = 0;
  
  Serial.println("[ESP32FirmwareDownloader] Starting OTA0 partition dump...");
  while (offset < endOffset) {
    uint32_t toRead = ((endOffset - offset) < OTAChunkSize) ? (endOffset - offset) : OTAChunkSize;
    esp_err_t err = esp_flash_read(esp_flash_default_chip, buffer, offset, toRead);
    if (err != ESP_OK) {
      Serial.printf("[ESP32FirmwareDownloader] Error reading OTA0 at offset %u, error: %d\n", offset, err);
      file.close();
      request->send(500, "text/plain", "Error reading OTA0 partition data");
      return;
    }
    file.write(buffer, toRead);
    offset += toRead;
    chunkCounter++;
    yield();
    delay(5);
    esp_task_wdt_reset();
    if (chunkCounter % 10 == 0 || offset >= endOffset) {
      Serial.printf("[ESP32FirmwareDownloader] OTA0: Dumped %u/%u bytes...\n", offset - ota0->address, partSize);
    }
  }
  file.close();
  Serial.println("[ESP32FirmwareDownloader] OTA0 partition dump completed.");
  
  AsyncWebServerResponse* response = request->beginResponse(SD, filename, "application/octet-stream");
  response->addHeader("Content-Disposition", "attachment; filename=ota0.bin");
  Serial.printf("[ESP32FirmwareDownloader] Serving OTA0 file for download.\n");
  request->send(response);
}

void ESP32FirmwareDownloader::handleDownloadOTA1(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Download OTA1 request received.");
  // Find OTA_1 partition.
  const esp_partition_t* ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  if (ota1 == NULL) {
    Serial.println("[ESP32FirmwareDownloader] OTA1 partition not found!");
    request->send(404, "text/plain", "OTA1 partition not found");
    return;
  }
  uint32_t partSize = ota1->size;
  Serial.printf("[ESP32FirmwareDownloader] OTA1 partition found: label=%s, size=%u bytes\n", ota1->label, partSize);
  
  String filename = "/ota1.bin";
  if (SD.exists(filename)) {
    SD.remove(filename);
  }
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    request->send(500, "text/plain", "Failed to open OTA1 file on SD card");
    return;
  }
  
  uint32_t offset = ota1->address; // absolute flash address
  uint32_t endOffset = ota1->address + partSize;
  uint8_t buffer[OTAChunkSize];
  uint32_t chunkCounter = 0;
  
  Serial.println("[ESP32FirmwareDownloader] Starting OTA1 partition dump...");
  while (offset < endOffset) {
    uint32_t toRead = ((endOffset - offset) < OTAChunkSize) ? (endOffset - offset) : OTAChunkSize;
    esp_err_t err = esp_flash_read(esp_flash_default_chip, buffer, offset, toRead);
    if (err != ESP_OK) {
      Serial.printf("[ESP32FirmwareDownloader] Error reading OTA1 at offset %u, error: %d\n", offset, err);
      file.close();
      request->send(500, "text/plain", "Error reading OTA1 partition data");
      return;
    }
    file.write(buffer, toRead);
    offset += toRead;
    chunkCounter++;
    yield();
    delay(5);
    esp_task_wdt_reset();
    if (chunkCounter % 10 == 0 || offset >= endOffset) {
      Serial.printf("[ESP32FirmwareDownloader] OTA1: Dumped %u/%u bytes...\n", offset - ota1->address, partSize);
    }
  }
  file.close();
  Serial.println("[ESP32FirmwareDownloader] OTA1 partition dump completed.");
  
  AsyncWebServerResponse* response = request->beginResponse(SD, filename, "application/octet-stream");
  response->addHeader("Content-Disposition", "attachment; filename=ota1.bin");
  Serial.printf("[ESP32FirmwareDownloader] Serving OTA1 file for download.\n");
  request->send(response);
}
