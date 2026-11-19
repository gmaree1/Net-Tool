// Passive 2.4 GHz Wi-Fi scan worker, sorted access-point results, and display
// formatting for SSIDs, security, signal strength, channels, and PHY modes.
#include "wifi_scanner.h"
#include "power.h"
#include "esp_wifi.h"
#include <atomic>
#include <stdlib.h>

static WifiScanState sState = WIFI_SURVEY_OFF;
static bool sActive = false, sInitialized = false, sRadioOn = false, sWaiting = false;
static wifi_ap_record_t *sAps = nullptr;
static uint16_t sCount = 0;
static uint32_t sFinishedAt = 0;
static esp_err_t sError = ESP_OK;

// The worker owns these until its release-store. Only the UI thread owns sAps.
// Closing during a scan defers shutdown until it finishes, avoiding scan/result races.
static std::atomic<bool> sWorkerRunning{false};
static wifi_ap_record_t *sNextAps = nullptr;
static uint16_t sNextCount = 0;
static esp_err_t sNextError = ESP_OK;

static int compareAps(const void *left, const void *right) {
  const auto &a = *static_cast<const wifi_ap_record_t *>(left);
  const auto &b = *static_cast<const wifi_ap_record_t *>(right);
  int signalOrder = int(b.rssi) - int(a.rssi);
  return signalOrder ? signalOrder : memcmp(a.bssid, b.bssid, sizeof a.bssid);
}

static void scanTask(void *) {
  wifi_scan_config_t cfg = {};
  cfg.show_hidden = true;
  cfg.scan_type = WIFI_SCAN_TYPE_PASSIVE;
  cfg.scan_time.passive = 300;              // all locally permitted channels; listen only
  sNextError = esp_wifi_scan_start(&cfg, true); // blocks this worker, never the UI
  sNextCount = 0;
  sNextAps = nullptr;
  if (sNextError == ESP_OK) sNextError = esp_wifi_scan_get_ap_num(&sNextCount);
  if (sNextError == ESP_OK && sNextCount) {
    sNextAps = static_cast<wifi_ap_record_t *>(calloc(sNextCount, sizeof(wifi_ap_record_t)));
    if (!sNextAps) sNextError = ESP_ERR_NO_MEM;
    else sNextError = esp_wifi_scan_get_ap_records(&sNextCount, sNextAps);
  }
  // Clear the driver's list on every path, including no results / allocation failure.
  esp_wifi_clear_ap_list();
  if (sNextError != ESP_OK) {
    free(sNextAps);
    sNextAps = nullptr;
    sNextCount = 0;
  } else if (sNextCount > 1) {
    qsort(sNextAps, sNextCount, sizeof(wifi_ap_record_t), compareAps);
  }
  sWorkerRunning.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

static void releaseSurvey() {
  free(sAps);
  sAps = nullptr;
  sCount = 0;
  if (sRadioOn) {
    esp_wifi_stop();
    sRadioOn = false;
  }
  sState = WIFI_SURVEY_OFF;
  powerWifiBoost(false);                          // restore the 80 MHz battery clock
}

bool wifiScannerUpdate() {
  if (!sWaiting || sWorkerRunning.load(std::memory_order_acquire)) return false;
  sWaiting = false;
  free(sAps);
  sAps = sNextAps;
  sNextAps = nullptr;
  sCount = sNextCount;
  sError = sNextError;
  sFinishedAt = millis();
  sState = sError == ESP_OK ? WIFI_SURVEY_READY : WIFI_SURVEY_FAILED;
  if (!sActive) releaseSurvey();
  return true;
}

void wifiScannerRefresh() {
  wifiScannerUpdate();
  if (!sActive || sWaiting) return;
  free(sAps);
  sAps = nullptr;
  sCount = 0;
  sError = ESP_OK;
  if (!sInitialized) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    sError = esp_wifi_init(&cfg);
    if (sError == ESP_OK) {
      sInitialized = true;
      sError = esp_wifi_set_storage(WIFI_STORAGE_RAM);
      if (sError == ESP_OK) sError = esp_wifi_set_mode(WIFI_MODE_STA);
      if (sError != ESP_OK) {
        esp_wifi_deinit();
        sInitialized = false;
      }
    }
  }
  if (sError == ESP_OK && !sRadioOn) {
    sError = esp_wifi_start();
    sRadioOn = sError == ESP_OK;
  }
  if (sError != ESP_OK) { sState = WIFI_SURVEY_FAILED; return; }
  sWaiting = true;
  sWorkerRunning.store(true, std::memory_order_relaxed);
  sState = WIFI_SURVEY_SCANNING;
  if (xTaskCreate(scanTask, "wifi_survey", 4096, nullptr, 1, nullptr) != pdPASS) {
    sWorkerRunning.store(false, std::memory_order_relaxed);
    sWaiting = false;
    sError = ESP_ERR_NO_MEM;
    sState = WIFI_SURVEY_FAILED;
  }
}

void wifiScannerOpen() {
  sActive = true;
  powerWifiBoost(true);                           // Wi-Fi scan is happier above 80 MHz
  if (!sWaiting) wifiScannerRefresh();
}

void wifiScannerClose() {
  sActive = false;
  wifiScannerUpdate();
  if (!sWaiting) releaseSurvey();
}

WifiScanState wifiScannerState() { return sState; }
int wifiScannerCount() { return sCount; }
const wifi_ap_record_t *wifiScannerAp(int index) {
  return index >= 0 && index < sCount ? &sAps[index] : nullptr;
}
uint32_t wifiScannerAgeSeconds() { return (millis() - sFinishedAt) / 1000; }
const char *wifiScannerError() { return esp_err_to_name(sError); }

String wifiSsid(const wifi_ap_record_t &ap) {
  if (!ap.ssid[0]) return "Hidden network";
  String label;
  for (size_t i = 0; i < 32 && ap.ssid[i]; ++i)
    label += (ap.ssid[i] >= 32 && ap.ssid[i] <= 126) ? char(ap.ssid[i]) : '?';
  return label;
}

const char *wifiAuthName(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA Personal";
    case WIFI_AUTH_WPA2_PSK: return "WPA2 Personal";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA / WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2 Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3 Personal";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2 / WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    case WIFI_AUTH_WPA3_ENT_192: return "WPA3 Enterprise 192";
    default: return "Unknown";
  }
}

const char *wifiCipherName(wifi_cipher_type_t cipher) {
  switch (cipher) {
    case WIFI_CIPHER_TYPE_NONE: return "None";
    case WIFI_CIPHER_TYPE_WEP40: return "WEP-40";
    case WIFI_CIPHER_TYPE_WEP104: return "WEP-104";
    case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP: return "AES-CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP + CCMP";
    case WIFI_CIPHER_TYPE_AES_CMAC128: return "AES-CMAC-128";
    case WIFI_CIPHER_TYPE_SMS4: return "SMS4";
    case WIFI_CIPHER_TYPE_GCMP: return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256: return "GCMP-256";
    case WIFI_CIPHER_TYPE_AES_GMAC128: return "AES-GMAC-128";
    case WIFI_CIPHER_TYPE_AES_GMAC256: return "AES-GMAC-256";
    default: return "Unknown";
  }
}

const char *wifiSignalName(int rssi) {
  if (rssi >= -55) return "Excellent";
  if (rssi >= -67) return "Good";
  if (rssi >= -75) return "Fair";
  return "Weak";
}
int wifiSignalPercent(int rssi) { return constrain(2 * (rssi + 100), 0, 100); }
int wifiChannelMHz(int channel) {
  if (channel == 14) return 2484;
  return channel >= 1 && channel <= 13 ? 2407 + 5 * channel : 0;
}
String wifiPhyName(const wifi_ap_record_t &ap) {
  String modes;
  if (ap.phy_11b) modes = "b";
  if (ap.phy_11g) modes += modes.length() ? "/g" : "g";
  if (ap.phy_11n) modes += modes.length() ? "/n" : "n";
  if (ap.phy_lr) modes += modes.length() ? "/LR" : "LR";
  return modes.length() ? "802.11 " + modes : String("Not reported");
}
