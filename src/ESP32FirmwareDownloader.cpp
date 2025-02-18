#include "ESP32FirmwareDownloader.h"
#include <WiFi.h>
#include <SPI.h>
#include "esp_flash.h"       // esp_flash_read() and esp_flash_default_chip()
#include "esp_partition.h"   // Partition APIs
#include "esp_ota_ops.h"     // OTA update APIs
#include <esp_task_wdt.h>    // esp_task_wdt_reset()
#include <esp_err.h>

#ifndef ESP_IMAGE_HEADER_MAGIC
  #define ESP_IMAGE_HEADER_MAGIC 0xE9
#endif

// Fixed constants for bootloader download.
static const uint32_t BOOTLOADER_OFFSET = 0x1000;
static const uint32_t BOOTLOADER_SIZE   = 0x7000;
static const size_t   CHUNK_SIZE        = 4096;

// Global variables for streaming.
static uint32_t g_flashSizeDirect = 0;
static uint32_t g_genPartStart = 0;
static uint32_t g_genPartSize  = 0;

// Forward declarations for helper functions.
static bool isPartitionValid(const esp_partition_t* part);
static bool cloneActiveToInactive();

// Initialize static members.
ESP32FirmwareDownloader* ESP32FirmwareDownloader::_instance = nullptr;
ESP32FirmwareDownloader::BlankRegion ESP32FirmwareDownloader::_blankRegions[MAX_BLANK_REGIONS];
int ESP32FirmwareDownloader::_numBlankRegions = 0;

////////////////////
// Helper Functions
////////////////////

// Check if a partition appears valid by reading its first byte.
static bool isPartitionValid(const esp_partition_t* part) {
  uint8_t magic;
  esp_err_t err = esp_flash_read(esp_flash_default_chip, &magic, part->address, 1);
  if (err != ESP_OK) {
    Serial.printf("[isPartitionValid] Error reading flash at 0x%08X, error: %d\n", part->address, err);
    return false;
  }
  return (magic == ESP_IMAGE_HEADER_MAGIC);
}

// Clone the active APP partition to the inactive APP partition.
static bool cloneActiveToInactive() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running) {
    Serial.println("Failed to get running partition!");
    return false;
  }
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  const esp_partition_t *inactive = nullptr;
  while (it != NULL) {
    const esp_partition_t *p = esp_partition_get(it);
    if (p && (p->address != running->address)) {
      inactive = p;
      break;
    }
    it = esp_partition_next(it);
  }
  if (!inactive) {
    Serial.println("Inactive partition not found!");
    return false;
  }
  
  uint8_t firstByte;
  if (esp_flash_read(esp_flash_default_chip, &firstByte, inactive->address, 1) != ESP_OK) {
    Serial.println("Error reading inactive partition!");
    return false;
  }
  if (firstByte != ESP_IMAGE_HEADER_MAGIC) {
    Serial.println("Inactive partition appears empty; proceeding with clone.");
  } else {
    Serial.println("Inactive partition appears valid; cloning anyway.");
  }
  
  esp_ota_handle_t ota_handle;
  esp_err_t err = esp_ota_begin(inactive, running->size, &ota_handle);
  if (err != ESP_OK) {
    Serial.printf("esp_ota_begin failed (%s)!\n", esp_err_to_name(err));
    return false;
  }
  
  const size_t chunkSize = CHUNK_SIZE;
  uint8_t buffer[chunkSize];
  size_t bytesRead = 0;
  size_t totalSize = running->size;
  
  Serial.printf("Cloning %u bytes from 0x%08X to 0x%08X...\n", totalSize, running->address, inactive->address);
  
  while (bytesRead < totalSize) {
    size_t toRead = ((totalSize - bytesRead) < chunkSize) ? (totalSize - bytesRead) : chunkSize;
    err = esp_flash_read(esp_flash_default_chip, buffer, running->address + bytesRead, toRead);
    if (err != ESP_OK) {
      Serial.printf("esp_flash_read failed at offset %u (%s)!\n", bytesRead, esp_err_to_name(err));
      return false;
    }
    err = esp_ota_write(ota_handle, buffer, toRead);
    if (err != ESP_OK) {
      Serial.printf("esp_ota_write failed at offset %u (%s)!\n", bytesRead, esp_err_to_name(err));
      return false;
    }
    bytesRead += toRead;
    Serial.printf("Cloned %u/%u bytes...\n", bytesRead, totalSize);
    yield();
    esp_task_wdt_reset();
  }
  
  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    Serial.printf("esp_ota_end failed (%s)!\n", esp_err_to_name(err));
    return false;
  }
  
  err = esp_ota_set_boot_partition(inactive);
  if (err != ESP_OK) {
    Serial.printf("esp_ota_set_boot_partition failed (%s)!\n", esp_err_to_name(err));
    return false;
  }
  
  Serial.println("Clone complete; inactive partition activated.");
  return true;
}

//////////////////////////
// Streaming Callback Functions
//////////////////////////

size_t ESP32FirmwareDownloader::flashStreamCallback(uint8_t *buffer, size_t maxLen, size_t index) {
  if (index >= g_flashSizeDirect) return 0;
  size_t bytesToRead = ((g_flashSizeDirect - index) < maxLen) ? (g_flashSizeDirect - index) : maxLen;
  esp_err_t err = esp_flash_read(esp_flash_default_chip, buffer, index, bytesToRead);
  if (err != ESP_OK) {
    Serial.printf("[DirectStream] Error at 0x%08X: %s\n", index, esp_err_to_name(err));
    return 0;
  }
  static uint32_t lastPrinted = 0;
  if (index - lastPrinted >= CHUNK_SIZE * 10) {
    Serial.printf("[DirectStream] Streamed %u/%u bytes...\n", index + bytesToRead, g_flashSizeDirect);
    lastPrinted = index;
  }
  esp_task_wdt_reset();
  return bytesToRead;
}

size_t ESP32FirmwareDownloader::flashStreamCallbackBlanked(uint8_t *buffer, size_t maxLen, size_t index) {
  if (index >= g_flashSizeDirect) return 0;
  size_t bytesToRead = ((g_flashSizeDirect - index) < maxLen) ? (g_flashSizeDirect - index) : maxLen;
  // Limit bytes to CHUNK_SIZE.
  if (bytesToRead > CHUNK_SIZE) bytesToRead = CHUNK_SIZE;
  uint8_t tempBuffer[CHUNK_SIZE];
  esp_err_t err = esp_flash_read(esp_flash_default_chip, tempBuffer, index, bytesToRead);
  if (err != ESP_OK) {
    Serial.printf("[SecureStream] Error at 0x%08X: %s\n", index, esp_err_to_name(err));
    return 0;
  }
  // For each blank region, replace overlapping bytes with 0xFF.
  for (int i = 0; i < _numBlankRegions; i++) {
    uint32_t regionStart = _blankRegions[i].offset;
    uint32_t regionEnd = regionStart + _blankRegions[i].length;
    uint32_t chunkStart = index;
    uint32_t chunkEnd = index + bytesToRead;
    if (chunkEnd > regionStart && chunkStart < regionEnd) {
      uint32_t overlapStart = (chunkStart > regionStart) ? chunkStart : regionStart;
      uint32_t overlapEnd = (chunkEnd < regionEnd) ? chunkEnd : regionEnd;
      uint32_t startInBuffer = overlapStart - chunkStart;
      uint32_t overlapLen = overlapEnd - overlapStart;
      for (uint32_t j = 0; j < overlapLen; j++) {
        tempBuffer[startInBuffer + j] = 0xFF;
      }
      Serial.printf("[SecureStream] Applied blank region: %s (0x%08X - 0x%08X)\n",
                    _blankRegions[i].description, regionStart, regionEnd);
    }
  }
  memcpy(buffer, tempBuffer, bytesToRead);
  static uint32_t lastPrinted = 0;
  if (index - lastPrinted >= CHUNK_SIZE * 10) {
    Serial.printf("[SecureStream] Streamed %u/%u bytes...\n", index + bytesToRead, g_flashSizeDirect);
    lastPrinted = index;
  }
  esp_task_wdt_reset();
  return bytesToRead;
}

size_t ESP32FirmwareDownloader::genericPartStreamCallback(uint8_t *buffer, size_t maxLen, size_t index) {
  if (index >= g_genPartSize) return 0;
  size_t bytesToRead = ((g_genPartSize - index) < maxLen) ? (g_genPartSize - index) : maxLen;
  esp_err_t err = esp_flash_read(esp_flash_default_chip, buffer, g_genPartStart + index, bytesToRead);
  if (err != ESP_OK) {
    Serial.printf("[GenericStream] Error at 0x%08X: %s\n", g_genPartStart + index, esp_err_to_name(err));
    return 0;
  }
  static uint32_t lastPrinted = 0;
  if (index - lastPrinted >= CHUNK_SIZE * 10) {
    Serial.printf("[GenericStream] Streamed %u/%u bytes...\n", index + bytesToRead, g_genPartSize);
    lastPrinted = index;
  }
  esp_task_wdt_reset();
  return bytesToRead;
}

//////////////////////////////
// Blank Region Management
//////////////////////////////
void ESP32FirmwareDownloader::addBlankRegion(uint32_t offset, uint32_t length, const char* description) {
  if (_numBlankRegions < MAX_BLANK_REGIONS) {
    _blankRegions[_numBlankRegions].offset = offset;
    _blankRegions[_numBlankRegions].length = length;
    _blankRegions[_numBlankRegions].description = description;
    _numBlankRegions++;
    Serial.printf("[ESP32FirmwareDownloader] Added blank region: 0x%08X - 0x%08X (%s)\n", offset, offset+length, description);
  } else {
    Serial.println("[ESP32FirmwareDownloader] Maximum blank regions reached.");
  }
}

//////////////////////////
// Constructor and Setters
//////////////////////////
ESP32FirmwareDownloader::ESP32FirmwareDownloader(const char* endpoint, const String &filename)
  : _endpoint(endpoint),
    _firmwareFilename(filename),
    _blankOffset(0),
    _blankLength(0)
{
  _instance = this;
  _numBlankRegions = 0;
}

void ESP32FirmwareDownloader::setFilename(const String &filename) {
  _firmwareFilename = filename;
}

void ESP32FirmwareDownloader::setBlankRegion(uint32_t offset, uint32_t length) {
  _blankOffset = offset;
  _blankLength = length;
  _numBlankRegions = 0;
  addBlankRegion(offset, length, "manual");
}

bool ESP32FirmwareDownloader::autoSetUserDataBlank() {
  const esp_partition_t* userPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "userdata");
  if (userPart != NULL) {
    Serial.printf("[ESP32FirmwareDownloader] Found user data partition '%s' at 0x%08X, size: %u bytes\n",
                  userPart->label, userPart->address, userPart->size);
    _numBlankRegions = 0;
    addBlankRegion(userPart->address, userPart->size, "userdata");
    return true;
  }
  Serial.println("[ESP32FirmwareDownloader] No user data partition found with label 'userdata'.");
  return false;
}

bool ESP32FirmwareDownloader::autoSetUserDataBlankAll() {
  const char* labelsToBlank[] = {"nvs", "spiffs", "littlefs"};
  bool found = false;
  for (int i = 0; i < 3; i++) {
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, labelsToBlank[i]);
    if (part) {
      addBlankRegion(part->address, part->size, labelsToBlank[i]);
      found = true;
    }
  }
  if (!found) {
    Serial.println("[ESP32FirmwareDownloader] No NVS/Spiffs/LittleFS partitions found for blanking.");
  }
  return found;
}

////////////////////////////
// HTTP Handlers
////////////////////////////

void ESP32FirmwareDownloader::handleDumpFlash(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Full flash dump request received.");
  uint32_t flashSize = ESP.getFlashChipSize();
  Serial.printf("[ESP32FirmwareDownloader] Flash size: %u bytes\n", flashSize);
  g_flashSizeDirect = flashSize;
  
  AsyncWebServerResponse *response = request->beginChunkedResponse("application/octet-stream", flashStreamCallback);
  response->addHeader("Content-Disposition", "attachment; filename=fullclone.bin");
  Serial.println("[ESP32FirmwareDownloader] Streaming full flash dump...");
  request->send(response);
}

void ESP32FirmwareDownloader::handleDumpFlashSecure(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Secure full flash dump request received.");
  uint32_t flashSize = ESP.getFlashChipSize();
  Serial.printf("[ESP32FirmwareDownloader] Flash size: %u bytes\n", flashSize);
  g_flashSizeDirect = flashSize;
  
  AsyncWebServerResponse *response = request->beginChunkedResponse("application/octet-stream", flashStreamCallbackBlanked);
  response->addHeader("Content-Disposition", "attachment; filename=fullclone_secure.bin");
  Serial.println("[ESP32FirmwareDownloader] Streaming secure full flash dump...");
  request->send(response);
}

void ESP32FirmwareDownloader::handleDownloadPartitionDirect(AsyncWebServerRequest *request) {
  if (!request->hasParam("label")) {
    request->send(400, "text/plain", "Missing 'label' parameter");
    return;
  }
  String label = request->getParam("label")->value();
  Serial.printf("[ESP32FirmwareDownloader] Direct partition download for label: %s\n", label.c_str());
  
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label.c_str());
  if (!part) {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label.c_str());
  }
  if (!part) {
    request->send(404, "text/plain", "Partition not found");
    return;
  }
  Serial.printf("[ESP32FirmwareDownloader] Partition %s found, size %u bytes\n", part->label, part->size);
  
  g_genPartStart = part->address;
  g_genPartSize  = part->size;
  
  AsyncWebServerResponse* response = request->beginChunkedResponse("application/octet-stream", genericPartStreamCallback);
  response->addHeader("Content-Disposition", "attachment; filename=" + label + ".bin");
  Serial.println("[ESP32FirmwareDownloader] Streaming generic partition...");
  request->send(response);
}

void ESP32FirmwareDownloader::handleDownloadBoot(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Bootloader download request received.");
  AsyncWebServerResponse* response = request->beginChunkedResponse("application/octet-stream",
    [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      if (index >= BOOTLOADER_SIZE) return 0;
      size_t toRead = ((BOOTLOADER_SIZE - index) < maxLen) ? (BOOTLOADER_SIZE - index) : maxLen;
      esp_err_t err = esp_flash_read(esp_flash_default_chip, buffer, BOOTLOADER_OFFSET + index, toRead);
      if (err != ESP_OK) {
        Serial.printf("[BootloaderStream] Error at 0x%08X: %s\n", BOOTLOADER_OFFSET + index, esp_err_to_name(err));
        return 0;
      }
      return toRead;
    });
  response->addHeader("Content-Disposition", "attachment; filename=bootloader.bin");
  Serial.println("[ESP32FirmwareDownloader] Streaming bootloader...");
  request->send(response);
}

void ESP32FirmwareDownloader::handleActivatePartition(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Activate partition request received.");
  const esp_partition_t* current = esp_ota_get_running_partition();
  const esp_partition_t* target = nullptr;
  
  if (request->hasParam("label")) {
    String label = request->getParam("label")->value();
    target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label.c_str());
    if (!target) {
      request->send(404, "text/plain", "Specified partition not found");
      return;
    }
    if (target->address == current->address) {
      request->send(400, "text/plain", "Specified partition is already running");
      return;
    }
  } else {
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
      const esp_partition_t* p = esp_partition_get(it);
      if (p && (p->address != current->address)) {
        target = p;
        break;
      }
      it = esp_partition_next(it);
    }
    if (!target) {
      request->send(500, "text/plain", "Inactive partition not found");
      return;
    }
  }
  
  if (!isPartitionValid(target)) {
    request->send(400, "text/plain", "Partition appears empty/unavailable");
    return;
  }
  
  esp_err_t err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    String errMsg = "Failed to set boot partition: ";
    errMsg += esp_err_to_name(err);
    request->send(500, "text/plain", errMsg);
    return;
  }
  
  String msg = "Partition ";
  msg += target->label;
  msg += " activated. Rebooting now...";
  request->send(200, "text/plain", msg);
  Serial.println("[ESP32FirmwareDownloader] Partition activated. Rebooting...");
  delay(2000);
  esp_restart();
}

void ESP32FirmwareDownloader::handleClonePartition(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Clone partition request received.");
  if (cloneActiveToInactive()) {
    request->send(200, "text/plain", "Clone successful.");
  } else {
    request->send(500, "text/plain", "Clone failed.");
  }
}

void ESP32FirmwareDownloader::handleRoot(AsyncWebServerRequest *request) {
  Serial.println("[ESP32FirmwareDownloader] Sending FWDL root page with device metadata and partition map.");

  const esp_partition_t* running = esp_ota_get_running_partition();
  String chipModel = ESP.getChipModel();
  String chipRevision = String(ESP.getChipRevision());
  float flashMB = ESP.getFlashChipSize() / (1024.0 * 1024.0);
  String cpuFreq = String(ESP.getCpuFreqMHz());

  String htmlHeader = R"rawliteral(
<html>
  <head>
    <title>ESP32 Firmware Download (FWDL)</title>
    <style>
      body { font-family: Arial, sans-serif; margin: 20px; }
      table { border-collapse: collapse; width: 90%; }
      th, td { border: 1px solid #ccc; padding: 8px; text-align: center; }
      th { background-color: #f2f2f2; }
      .highlight { background-color: #c0ffc0; }
      .nav { margin-top: 20px; }
      .nav a { margin-right: 10px; text-decoration: none; color: blue; }
      .nav a:hover { text-decoration: underline; }
    </style>
    <script>
      document.addEventListener("DOMContentLoaded", function() {
        var table = document.querySelector("table");
        if (table) {
          table.addEventListener("mouseover", function(e) {
            var target = e.target;
            if (target && target.hasAttribute("data-index")) {
              var idx = target.getAttribute("data-index");
              var row = target.closest("tr");
              if (row) {
                var cells = row.querySelectorAll('[data-index="' + idx + '"]');
                cells.forEach(function(cell) { cell.classList.add("highlight"); });
              }
            }
          });
          table.addEventListener("mouseout", function(e) {
            var target = e.target;
            if (target && target.hasAttribute("data-index")) {
              var idx = target.getAttribute("data-index");
              var row = target.closest("tr");
              if (row) {
                var cells = row.querySelectorAll('[data-index="' + idx + '"]');
                cells.forEach(function(cell) { cell.classList.remove("highlight"); });
              }
            }
          });
        }
      });
    </script>
  </head>
  <body>
    <h1>ESP32 Firmware Download (FWDL)</h1>
    <h2>Device Information</h2>
    <ul>
      <li>Chip Model: )rawliteral" + chipModel + R"rawliteral(</li>
      <li>Chip Revision: )rawliteral" + chipRevision + R"rawliteral(</li>
      <li>Flash Size: )rawliteral" + String(flashMB, 2) + R"rawliteral( MB</li>
      <li>CPU Frequency: )rawliteral" + cpuFreq + R"rawliteral( MHz</li>
    </ul>
    <h2>Partition Map</h2>
    <table>
      <tr>
        <th>Type</th>
        <th>Label</th>
        <th>Address</th>
        <th>Size (bytes)</th>
        <th>Download</th>
        <th>Activate</th>
        <th>Upload</th>
      </tr>
)rawliteral";

  String rows = "";
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL) {
    const esp_partition_t* p = esp_partition_get(it);
    bool isRunning = (running && (p->address == running->address));
    String rowStyle = isRunning ? " style=\"background-color:yellow;\"" : "";
    String displayLabel = String(p->label);
    if (isRunning) displayLabel += " (running)";
    char addrStr[20];
    sprintf(addrStr, "0x%08X", p->address);

    rows += "<tr" + rowStyle + ">";
    rows += "<td>APP</td>";
    rows += "<td>" + displayLabel + "</td>";
    rows += "<td>" + String(addrStr) + "</td>";
    rows += "<td>" + String(p->size) + "</td>";
    rows += "<td><a href=\"/downloaddirect?label=" + String(p->label) + "\">Download</a></td>";
    if (!isRunning) {
      if (isPartitionValid(p)) {
        rows += "<td><button onclick=\"location.href='/activate?label=" + String(p->label) + "'\">Activate</button></td>";
      } else {
        rows += "<td><button disabled title=\"Partition unavailable\">Activate</button></td>";
      }
      rows += "<td><form method='POST' action='/upload' enctype='multipart/form-data' style='display:inline;'>"
              "<input type='hidden' name='label' value='" + String(p->label) + "'>"
              "<input type='file' name='file' style='width:150px;' onchange='checkAppImage(this, " + String(p->size) + ")'>"
              "<input type='submit' value='Upload'>"
              "</form></td>";
    } else {
      rows += "<td>N/A</td><td>N/A</td>";
    }
    rows += "</tr>";
    it = esp_partition_next(it);
  }

  it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL) {
    const esp_partition_t* p = esp_partition_get(it);
    char addrStr[20];
    sprintf(addrStr, "0x%08X", p->address);
    rows += "<tr>";
    rows += "<td>DATA</td>";
    rows += "<td>" + String(p->label) + "</td>";
    rows += "<td>" + String(addrStr) + "</td>";
    rows += "<td>" + String(p->size) + "</td>";
    rows += "<td><a href=\"/downloaddirect?label=" + String(p->label) + "\">Download</a></td>";
    rows += "<td>N/A</td>";
    rows += "<td><form method='POST' action='/upload' enctype='multipart/form-data' style='display:inline;'>"
            "<input type='hidden' name='label' value='" + String(p->label) + "'>"
            "<input type='file' name='file' style='width:150px;' onchange='checkFileSize(this, " + String(p->size) + ")'>"
            "<input type='submit' value='Upload'>"
            "</form></td>";
    rows += "</tr>";
    it = esp_partition_next(it);
  }

  String htmlFooter = R"rawliteral(
    </table>
    <h2>Global Download Links</h2>
    <ul>
      <li><a href="/dumpflash">Full Flash Dump</a></li>
      <li><a href="/dumpflash_secure">Secure Full Flash Dump</a></li>
      <li><a href="/downloadboot">Bootloader Download</a></li>
      <li><a href="/clone">Clone Active APP Partition</a></li>
      <li>Generic Download: /downloaddirect?label=YourPartitionLabel</li>
    </ul>
  </body>
</html>
)rawliteral";

  String fullHtml = htmlHeader + rows + htmlFooter;
  request->send(200, "text/html", fullHtml);
}

//////////////////////////////
// Upload Handler
//////////////////////////////
void ESP32FirmwareDownloader::handleUploadBinary(AsyncWebServerRequest *request,
    const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
  // Retrieve target partition label.
  String label = "";
  if (request->hasParam("label", true)) {
    label = request->getParam("label", true)->value();
  } else {
    Serial.println("[Upload] Missing 'label' parameter");
    request->send(400, "text/plain", "Missing 'label' parameter");
    return;
  }

  // Try to find APP partition first.
  const esp_partition_t* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label.c_str());
  // Static variables for APP OTA update.
  static esp_ota_handle_t ota_handle = 0;
  static bool otaStarted = false;
  
  // For DATA partitions.
  static bool dataUpdateStarted = false;
  static uint32_t dataOffset = 0;

  if (!target) {
    target = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label.c_str());
    if (!target) {
      Serial.printf("[Upload] Target partition '%s' not found!\n", label.c_str());
      request->send(404, "text/plain", "Target partition not found");
      return;
    }
    if (index == 0) {
      dataUpdateStarted = true;
      dataOffset = 0;
      Serial.printf("[Upload] Erasing DATA partition '%s' (size: %u bytes)...\n", label.c_str(), target->size);
      esp_err_t err = esp_partition_erase_range(target, 0, target->size);
      if (err != ESP_OK) {
        Serial.printf("[Upload] Erase failed: %s\n", esp_err_to_name(err));
        request->send(500, "text/plain", "Erase partition failed");
        dataUpdateStarted = false;
        return;
      }
    }
    esp_err_t err = esp_partition_write(target, dataOffset, data, len);
    if (err != ESP_OK) {
      Serial.printf("[Upload] esp_partition_write failed at offset %u: %s\n", dataOffset, esp_err_to_name(err));
      return;
    }
    dataOffset += len;
    if (final) {
      Serial.printf("[Upload] DATA partition '%s' update complete.\n", label.c_str());
      request->send(200, "text/plain", "Upload complete for DATA partition");
      dataUpdateStarted = false;
    }
    return;
  }
  
  // For APP partitions, disallow updating the active partition.
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && target->address == running->address) {
    Serial.printf("[Upload] Cannot update active partition '%s'.\n", label.c_str());
    request->send(400, "text/plain", "Cannot update active partition");
    return;
  }
  
  // APP OTA update.
  if (index == 0) {
    if (otaStarted) {
      Serial.println("[Upload] OTA update already started, ignoring new start.");
      return;
    }
    otaStarted = true;
    Serial.printf("[Upload] Beginning OTA update for partition '%s' (size: %u bytes)...\n", label.c_str(), target->size);
    esp_err_t err = esp_ota_begin(target, target->size, &ota_handle);
    if (err != ESP_OK) {
      Serial.printf("[Upload] esp_ota_begin failed: %s\n", esp_err_to_name(err));
      ota_handle = 0;
      otaStarted = false;
      request->send(500, "text/plain", "OTA update failed to begin");
      return;
    }
  }
  
  if (ota_handle == 0) {
    Serial.println("[Upload] OTA handle not available.");
    return;
  }
  
  esp_err_t err = esp_ota_write(ota_handle, data, len);
  if (err != ESP_OK) {
    Serial.printf("[Upload] esp_ota_write failed at index %u: %s\n", index, esp_err_to_name(err));
    return;
  }
  
  if (final) {
    Serial.println("[Upload] Finalizing OTA update...");
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
      Serial.printf("[Upload] esp_ota_end failed: %s\n", esp_err_to_name(err));
      return;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
      Serial.printf("[Upload] esp_ota_set_boot_partition failed: %s\n", esp_err_to_name(err));
      return;
    }
    Serial.println("[Upload] OTA update complete. Rebooting...");
    request->send(200, "text/plain", "Upload complete, device will reboot");
    ota_handle = 0;
    otaStarted = false;
    delay(2000);
    esp_restart();
  }
}

////////////////////////////
// attach() and attachAll()
////////////////////////////
bool ESP32FirmwareDownloader::attach(AsyncWebServer &server, bool eraseUserData) {
  if (eraseUserData) {
    if (autoSetUserDataBlankAll()) {
      Serial.println("[ESP32FirmwareDownloader] User data partitions detected for secure dump.");
    } else {
      Serial.println("[ESP32FirmwareDownloader] No additional user data partitions found.");
    }
  }
  server.on(_endpoint, HTTP_GET, handleDumpFlash);
  return true;
}

bool ESP32FirmwareDownloader::attachAll(AsyncWebServer &server, bool eraseUserData) {
  bool ok = true;
  ok &= attach(server, eraseUserData);
  server.on("/downloadboot", HTTP_GET, handleDownloadBoot);
  server.on("/downloaddirect", HTTP_GET, handleDownloadPartitionDirect);
  server.on("/activate", HTTP_GET, handleActivatePartition);
  server.on("/clone", HTTP_GET, handleClonePartition);
  server.on("/FWDL", HTTP_GET, handleRoot);
  server.on("/dumpflash_secure", HTTP_GET, handleDumpFlashSecure);
  server.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->send(200, "text/plain", "Upload complete");
    },
    handleUploadBinary
  );
  return ok;
}
