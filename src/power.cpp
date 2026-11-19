// Option A battery power policy. Cable-sleep only runs with no link, so PoE
// (which always has a cable) is never disturbed. See README "Battery and power".
#include "power.h"
#include "net.h"
#include <M5Unified.h>
#include <Preferences.h>

// ---------------------------------------------------------------- tunables
const uint32_t POWER_LINK_DOWN_SLEEP_MS      = 15000;   // no link this long → PHY sleep
const uint32_t POWER_CABLE_PROBE_INTERVAL_MS = 30000;   // Gerhard: check every 30 s
const uint32_t POWER_CABLE_PROBE_WAIT_MS     = 3000;    // brief wake to detect link
const uint32_t POWER_DIM_AFTER_MS            = 30000;
const uint32_t POWER_DARK_AFTER_MS           = 120000;
const uint32_t POWER_AUTO_OFF_IDLE_MS        = 600000;  // 10 min, no cable, not charging
const uint32_t POWER_AUTO_OFF_COUNTDOWN_MS   = 20000;
const uint8_t  POWER_BRIGHTNESS_ACTIVE       = 120;
const uint8_t  POWER_BRIGHTNESS_DIM          = 30;
const uint32_t POWER_CPU_MHZ                 = 80;
const uint32_t POWER_CPU_WIFI_MHZ            = 160;

static Preferences sPrefs;
static bool        sEco10 = false;
static bool        sHoldAwake = false;
static bool        sWifiBoost = false;
static bool        sPhySleeping = false;
static bool        sProbing = false;
static PowerScreen sScreen = POWER_SCREEN_ACTIVE;
static uint32_t    sLastActivityMs = 0;
static uint32_t    sLinkDownSinceMs = 0;
static uint32_t    sLastProbeMs = 0;
static uint32_t    sProbeUntilMs = 0;
static uint32_t    sAutoOffDeadlineMs = 0;   // 0 = not counting down
static bool        sDirtyHint = true;

static void applyBrightness() {
  uint8_t level = POWER_BRIGHTNESS_ACTIVE;
  if (sScreen == POWER_SCREEN_DIM) level = POWER_BRIGHTNESS_DIM;
  else if (sScreen == POWER_SCREEN_DARK) level = 0;
  M5.Display.setBrightness(level);
}

static void applyCpu() {
  uint32_t mhz = sWifiBoost ? POWER_CPU_WIFI_MHZ : POWER_CPU_MHZ;
  if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);
}

static void setScreen(PowerScreen next) {
  if (sScreen == next) return;
  sScreen = next;
  applyBrightness();
  sDirtyHint = true;
}

static void wakeScreen() {
  sLastActivityMs = millis();
  sAutoOffDeadlineMs = 0;
  setScreen(POWER_SCREEN_ACTIVE);
}

static void startProbe(uint32_t now) {
  if (!sPhySleeping && !sProbing) return;
  netPhyPower(true);                      // restores Eco or auto-neg mode
  sPhySleeping = false;
  sProbing = true;
  sLastProbeMs = now;
  sProbeUntilMs = now + POWER_CABLE_PROBE_WAIT_MS;
  sDirtyHint = true;
  Serial.println("POWER: PHY probe for cable");
}

static void enterPhySleep(uint32_t now) {
  if (sHoldAwake || sProbing) return;
  netPhyPower(false);
  sPhySleeping = true;
  sProbing = false;
  sLastProbeMs = now;
  sDirtyHint = true;
  Serial.println("POWER: PHY cable-sleep");
}

void powerBegin() {
  sLastActivityMs = millis();
  sLinkDownSinceMs = millis();
  sLastProbeMs = millis();
  sPrefs.begin("nettool", false);
  sEco10 = sPrefs.getBool("eco10", false);
  // netSetEco10M is applied after netBegin() — the MAC is not ready yet here.
  setCpuFrequencyMhz(POWER_CPU_MHZ);
  M5.Display.setBrightness(POWER_BRIGHTNESS_ACTIVE);
  sScreen = POWER_SCREEN_ACTIVE;
  Serial.printf("POWER: CPU %lu MHz, backlight %u, Eco10M=%s\n",
                (unsigned long)POWER_CPU_MHZ, POWER_BRIGHTNESS_ACTIVE,
                sEco10 ? "on" : "off");
}

void powerApplyNet() {
  netSetEco10M(sEco10);
}

void powerNoteActivity() {
  wakeScreen();
  // Touch / button: check for a cable immediately when the PHY is sleeping.
  if (sPhySleeping || sProbing) startProbe(millis());
}

void powerNoteEvent() {
  wakeScreen();
}

void powerHoldAwake(bool hold) {
  sHoldAwake = hold;
  if (hold) {
    uint32_t now = millis();
    if (sPhySleeping || sProbing) startProbe(now);
    else if (!sPhySleeping) {
      // Ensure PHY is up for Blink / DHCP even if a race left it mid-transition.
      netPhyPower(true);
      sPhySleeping = false;
      sProbing = false;
    }
    sAutoOffDeadlineMs = 0;
    wakeScreen();
  }
}

void powerWifiBoost(bool on) {
  sWifiBoost = on;
  applyCpu();
}

bool powerShouldRender() { return sScreen != POWER_SCREEN_DARK; }
uint32_t powerRedrawIntervalMs() {
  return sScreen == POWER_SCREEN_ACTIVE ? 500 : 1000;
}
PowerScreen powerScreen() { return sScreen; }
bool powerPhySleeping() { return sPhySleeping && !sProbing; }
bool powerAutoOffPending() { return sAutoOffDeadlineMs != 0; }
uint32_t powerAutoOffSecondsLeft() {
  if (!sAutoOffDeadlineMs) return 0;
  int32_t left = (int32_t)(sAutoOffDeadlineMs - millis());
  return left > 0 ? (uint32_t)((left + 999) / 1000) : 0;
}
bool powerEco10M() { return sEco10; }

void powerSetEco10M(bool on) {
  if (sEco10 == on) return;
  sEco10 = on;
  sPrefs.putBool("eco10", on);
  netSetEco10M(on);
  sDirtyHint = true;
  Serial.printf("POWER: Eco 10M %s\n", on ? "on" : "off");
}

const char *powerStatusHint() {
  if (sPhySleeping && !sProbing) return "sleep · tap to check";
  if (sProbing) return "checking cable…";
  return "";
}

void powerTick(bool lanOk) {
  uint32_t now = millis();
  applyCpu();

  NetState st = {};
  if (lanOk) netGetState(st);

  if (!lanOk) {
    // No module: still dim / auto-off on battery, but no PHY work.
    sPhySleeping = false;
    sProbing = false;
  } else if (st.link) {
    // Never sleep the PHY while linked (covers PoE).
    if (sPhySleeping || sProbing) {
      sPhySleeping = false;
      sProbing = false;
      sDirtyHint = true;
    }
    sLinkDownSinceMs = 0;
    sAutoOffDeadlineMs = 0;
  } else {
    if (!sLinkDownSinceMs) sLinkDownSinceMs = now;

    if (sProbing) {
      if (st.link) {
        sProbing = false;
        sPhySleeping = false;
        sDirtyHint = true;
      } else if ((int32_t)(now - sProbeUntilMs) >= 0) {
        // No link during the probe window → back to sleep (unless a tool holds us).
        sProbing = false;
        if (!sHoldAwake) enterPhySleep(now);
        else sPhySleeping = false;
      }
    } else if (sPhySleeping) {
      if (sHoldAwake) startProbe(now);
      else if ((int32_t)(now - sLastProbeMs) >= (int32_t)POWER_CABLE_PROBE_INTERVAL_MS)
        startProbe(now);
    } else if (!sHoldAwake &&
               (int32_t)(now - sLinkDownSinceMs) >= (int32_t)POWER_LINK_DOWN_SLEEP_MS) {
      enterPhySleep(now);
    }
  }

  // ---- idle auto power-off: no cable, not charging, long idle
  // Touch cancels via powerNoteActivity() clearing sAutoOffDeadlineMs.
  uint32_t idle = now - sLastActivityMs;
  // No module counts as "no cable": still park the unit if left on battery.
  bool mayAutoOff = (!lanOk || !st.link) && !sHoldAwake && !M5.Power.isCharging();
  if (!mayAutoOff) {
    sAutoOffDeadlineMs = 0;
  } else if (idle >= POWER_AUTO_OFF_IDLE_MS) {
    if (!sAutoOffDeadlineMs) {
      sAutoOffDeadlineMs = now + POWER_AUTO_OFF_COUNTDOWN_MS;
      Serial.println("POWER: auto-off countdown started");
    } else if ((int32_t)(now - sAutoOffDeadlineMs) >= 0) {
      Serial.println("POWER: auto power-off");
      M5.Display.setBrightness(0);
      delay(50);
      M5.Power.powerOff();
      // If powerOff returns (e.g. USB holding the rail), cancel and continue.
      sAutoOffDeadlineMs = 0;
      sLastActivityMs = millis();
    }
  }

  // ---- backlight idle ladder (countdown forces ACTIVE so the warning is visible)
  if (sAutoOffDeadlineMs) setScreen(POWER_SCREEN_ACTIVE);
  else if (idle >= POWER_DARK_AFTER_MS) setScreen(POWER_SCREEN_DARK);
  else if (idle >= POWER_DIM_AFTER_MS) setScreen(POWER_SCREEN_DIM);
  else setScreen(POWER_SCREEN_ACTIVE);

  (void)sDirtyHint;
}
