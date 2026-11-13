// Scan-only 2.4 GHz survey. Never associates or creates an IP interface.
#pragma once
#include <Arduino.h>
#include "esp_wifi_types.h"

enum WifiScanState { WIFI_SURVEY_OFF, WIFI_SURVEY_SCANNING, WIFI_SURVEY_READY, WIFI_SURVEY_FAILED };

void wifiScannerOpen();
void wifiScannerRefresh();
void wifiScannerClose();
bool wifiScannerUpdate();                 // call from loop; true when state changes
WifiScanState wifiScannerState();
int wifiScannerCount();
const wifi_ap_record_t *wifiScannerAp(int index); // valid until refresh / close
uint32_t wifiScannerAgeSeconds();
const char *wifiScannerError();

String wifiSsid(const wifi_ap_record_t &ap); // safe printable label, including hidden SSIDs
const char *wifiAuthName(wifi_auth_mode_t auth);
const char *wifiCipherName(wifi_cipher_type_t cipher);
const char *wifiSignalName(int rssi);
int wifiSignalPercent(int rssi);           // approximate display scale, not a measured quality
int wifiChannelMHz(int channel);
String wifiPhyName(const wifi_ap_record_t &ap);
