// Battery power policy for Option A: cable-sleep PHY, backlight idle levels,
// CPU clock, Eco 10M preference, and idle auto power-off. Timings are constants
// at the top of power.cpp so they stay easy to tune on the bench.
#pragma once
#include <Arduino.h>

// Tunable idle / probe timings (milliseconds) and display / CPU levels.
// Defined in power.cpp; listed here for discoverability from the UI / tools.
extern const uint32_t POWER_LINK_DOWN_SLEEP_MS;
extern const uint32_t POWER_CABLE_PROBE_INTERVAL_MS;
extern const uint32_t POWER_CABLE_PROBE_WAIT_MS;
extern const uint32_t POWER_DIM_AFTER_MS;
extern const uint32_t POWER_DARK_AFTER_MS;
extern const uint32_t POWER_AUTO_OFF_IDLE_MS;
extern const uint32_t POWER_AUTO_OFF_COUNTDOWN_MS;
extern const uint8_t  POWER_BRIGHTNESS_ACTIVE;
extern const uint8_t  POWER_BRIGHTNESS_DIM;
extern const uint32_t POWER_CPU_MHZ;
extern const uint32_t POWER_CPU_WIFI_MHZ;

enum PowerScreen { POWER_SCREEN_ACTIVE, POWER_SCREEN_DIM, POWER_SCREEN_DARK };

void        powerBegin();                 // CPU, backlight default, load Eco preference
void        powerApplyNet();              // push Eco preference into the W5500 after netBegin
void        powerTick(bool lanOk);        // call from loop(); drives timers and PHY probes
void        powerNoteActivity();          // touch / button / tool launch: wake screen + PHY check
void        powerNoteEvent();             // link/IP/neighbour/tool event: wake screen only
void        powerHoldAwake(bool hold);    // tools that need the PHY (Blink, DHCP scan)
void        powerWifiBoost(bool on);      // raise CPU while the Wi-Fi scanner runs
bool        powerShouldRender();          // false while backlight is off
uint32_t    powerRedrawIntervalMs();      // 500 ms active, 1000 ms dimmed
PowerScreen powerScreen();
bool        powerPhySleeping();           // true between cable probes (PHY power-down)
bool        powerAutoOffPending();        // countdown visible
uint32_t    powerAutoOffSecondsLeft();
bool        powerEco10M();
void        powerSetEco10M(bool on);      // persists via Preferences when available
const char *powerStatusHint();            // short status-bar suffix while no link
