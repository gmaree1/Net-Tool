// Core2 application: touch navigation, Ethernet status, switch discovery,
// diagnostic results, QR handoff, and passive Wi-Fi survey pages.
#include <M5Unified.h>
#include "net.h"
#include "tools.h"
#include "wifi_scanner.h"

// ---------------------------------------------------------------- layout
static const int SCREEN_W = 320, SCREEN_H = 240;
static const int BAR_H = 28;                 // status bar
static const int TAB_Y = 196, TAB_H = 44;    // bottom tab bar
static const int BODY_Y = BAR_H + 2;

enum Page { PAGE_NET, PAGE_SWITCH, PAGE_TOOLS };
enum Overlay { OV_NONE, OV_CONSOLE, OV_QR, OV_EXTRA, OV_WIFI_LIST, OV_WIFI_DETAIL };

static M5Canvas canvas(&M5.Display);
static lgfx::LovyanGFX *g = &M5.Display;     // draw target (off-screen canvas when available)
static bool useCanvas = false;

static Page page = PAGE_NET;
static Overlay overlay = OV_NONE;
static bool dirty = true;
static bool lanOk = false;
static int conScroll = 0;                    // lines scrolled back from the newest
static int pressedTool = -1;
static uint32_t lastConVersion = 0;
static uint32_t vibrateOffAt = 0;
static int wifiListPage = 0;
static int wifiSelected = -1;
static int wifiDetailPage = 0;
static const int WIFI_ROWS = 4;

// Last polled data
static NetState st;
static Neighbor nb;
static const char *nbProto = "";
static bool haveNb = false;
static DhcpInfo dhcp;

struct ToolButton { const char *label; uint16_t color; ToolId id; };
static const ToolButton TOOL_BUTTONS[8] = {
  {"Ping gateway",       C_GREEN,  TOOL_PING_GW},
  {"Ping 8.8.8.8",        C_BLUE,   TOOL_PING_INET},
  {"Traceroute 8.8.8.8",  C_PURPLE, TOOL_TRACE},
  {"DNS + web check",     C_TEAL,   TOOL_DNS_WEB},
  {"Scan DHCP servers",   C_ORANGE, TOOL_DHCP_SCAN},
  {"Blink switch port",   C_PINK,   TOOL_BLINK_PORT},
  {"Show QR code",        C_CYAN,   TOOL_QR},
  {"Extra tools",        C_YELLOW, TOOL_EXTRA},
};

// ---------------------------------------------------------------- drawing helpers
static String fit(const String &s, int maxW) {
  if (g->textWidth(s) <= maxW) return s;
  String t = s;
  while (t.length() > 1 && g->textWidth(t + "..") > maxW) t.remove(t.length() - 1);
  return t + "..";
}

static void text(const String &s, int x, int y, uint16_t color, const lgfx::IFont *font,
                 textdatum_t datum = textdatum_t::top_left, int maxW = 0) {
  g->setFont(font);
  g->setTextColor(color);
  g->setTextDatum(datum);
  g->drawString(maxW ? fit(s, maxW) : s, x, y);
}

static void row(int y, const char *label, const String &value, uint16_t labelColor,
                uint16_t valueColor = C_TEXT) {
  text(label, 8, y, labelColor, &fonts::Font2);
  text(value, 92, y, valueColor, &fonts::Font2, textdatum_t::top_left, SCREEN_W - 96);
}

static void infoCard(int x, int y, int w, int h, const char *label, const String &value,
                     uint16_t accent) {
  g->fillRoundRect(x, y, w, h, 6, C_CARD);
  g->fillRect(x, y + 5, 3, h - 10, accent);
  text(label, x + 9, y + 4, accent, &fonts::Font0);
  text(value, x + 9, y + h - 5, C_TEXT, &fonts::FreeSansBold9pt7b,
       textdatum_t::bottom_left, w - 17);
}

static String vlanSummary() {
  String s;
  if (haveNb && nb.nativeVlan >= 0) s += "native " + String(nb.nativeVlan);
  if (haveNb && nb.voiceVlan >= 0) s += String(s.length() ? "  " : "") + "voice " + String(nb.voiceVlan);
  if (st.vlanTagCount) {
    s += String(s.length() ? "  " : "") + "tags ";
    for (int i = 0; i < st.vlanTagCount; i++) s += String(i ? "," : "") + String(st.vlanTags[i]);
  }
  return s;
}

static String leaseStr(uint32_t sec) {
  if (sec >= 86400) return String(sec / 86400) + "d " + String((sec % 86400) / 3600) + "h";
  if (sec >= 3600) return String(sec / 3600) + "h " + String((sec % 3600) / 60) + "m";
  return String(sec / 60) + "m";
}

// ---------------------------------------------------------------- status bar + tabs
static void drawStatusBar() {
  uint16_t bg;
  String left;
  if (!lanOk) { bg = C_RED; left = "NO LAN MODULE"; }
  else if (!st.link) { bg = C_RED; left = "NO LINK"; }
  else {
    bg = st.hasIp ? C_GREEN : C_ORANGE;
    left = String("LINK ") + st.speedMbps + "M " + (st.fullDuplex ? "FULL" : "HALF");
    if (!st.hasIp) left += "  NO IP";
  }
  g->fillRect(0, 0, SCREEN_W, BAR_H, bg);
  g->fillCircle(14, BAR_H / 2, 6, rgb(0, 0, 0));
  g->fillCircle(14, BAR_H / 2, 4, (st.link && (millis() / 500) % 2) ? C_TEXT : bg);
  text(left, 28, BAR_H / 2, rgb(0, 0, 0), &fonts::FreeSansBold9pt7b, textdatum_t::middle_left);
  int batt = M5.Power.getBatteryLevel();
  if (batt >= 0) {
    String b = String(batt) + "%" + (M5.Power.isCharging() ? "+" : "");
    text(b, SCREEN_W - 8, BAR_H / 2, rgb(0, 0, 0), &fonts::FreeSansBold9pt7b, textdatum_t::middle_right);
  }
}

static void bigButton(int x, int y, int w, int h, uint16_t color, bool filled, const char *label) {
  if (filled) g->fillRoundRect(x, y, w, h, 8, color);
  else {
    g->fillRoundRect(x, y, w, h, 8, C_CARD);
    g->drawRoundRect(x, y, w, h, 8, color);
  }
  text(label, x + w / 2, y + h / 2, filled ? rgb(0, 0, 0) : color, &fonts::FreeSansBold9pt7b,
       textdatum_t::middle_center);
}

static void drawTabs() {
  static const char *const names[3] = {"NETWORK", "SWITCH", "TOOLS"};
  static const uint16_t colors[3] = {C_CYAN, C_YELLOW, C_ORANGE};
  for (int i = 0; i < 3; i++)
    bigButton(3 + i * 106, TAB_Y + 2, 102, TAB_H - 4, colors[i], page == i, names[i]);
}

// ---------------------------------------------------------------- pages
// Big VLAN tile: green with the number once the switch announces it
static void vlanTile(int x, int y, int w, int h, const lgfx::IFont *numFont) {
  bool has = haveNb && nb.nativeVlan >= 0;
  int vid = has ? nb.nativeVlan : -1;
  const char *label = "VLAN";
  if (!has && st.vlanTagCount) { vid = st.vlanTags[0]; label = "VLAN TAG"; }   // fall back to a tag seen on the wire
  g->fillRoundRect(x, y, w, h, 10, vid >= 0 ? C_GREEN : C_CARD);
  if (vid < 0) g->drawRoundRect(x, y, w, h, 10, C_DIM);
  text(label, x + w / 2, y + 11, vid >= 0 ? rgb(0, 0, 0) : C_DIM, &fonts::Font2, textdatum_t::middle_center);
  String num = vid >= 0 ? String(vid) : String(st.link && !haveNb ? "..." : "?");
  text(num, x + w / 2, y + h / 2 + 9, vid >= 0 ? rgb(0, 0, 0) : C_DIM, numFont, textdatum_t::middle_center);
}

static void drawNetPage() {
  // Lead with the two things most useful at a glance: our address and VLAN.
  g->fillRoundRect(4, BODY_Y, 210, 56, 8, C_CARD);
  text("IP ADDRESS", 12, BODY_Y + 5, C_DIM, &fonts::Font2);
  if (st.hasIp) {
    text(ipToStr(st.ip), 12, BODY_Y + 32, C_CYAN, &fonts::FreeSansBold12pt7b,
         textdatum_t::middle_left, 194);
    text("/" + String(maskToPrefix(st.mask)) + "  " + ipToStr(st.mask), 12, BODY_Y + 48,
         C_TEXT, &fonts::Font2, textdatum_t::middle_left, 194);
  } else {
    const char *msg = !lanOk ? "No LAN module" : !st.link ? "Plug in a cable" : "Waiting for DHCP";
    text(msg, 12, BODY_Y + 35, st.link ? C_YELLOW : C_RED, &fonts::FreeSansBold9pt7b,
         textdatum_t::middle_left, 194);
  }
  vlanTile(220, BODY_Y, 96, 56, &fonts::FreeSansBold18pt7b);

  String dns = st.hasIp && !ipIsZero(st.dns1) ? ipToStr(st.dns1) : String("-");
  if (st.hasIp && !ipIsZero(st.dns2)) dns += "  " + ipToStr(st.dns2);
  String srv = "-";
  if (dhcp.valid) {
    srv = ipToStr(dhcp.server);
    if (!ipIsZero(dhcp.relay)) srv += "  relay " + ipToStr(dhcp.relay);
  } else if (st.hasIp) srv = "static / unknown";
  String lease = dhcp.valid ? leaseStr(dhcp.leaseSec) : String("-");

  infoCard(4, BODY_Y + 61, 152, 34, "GATEWAY", st.hasIp ? ipToStr(st.gw) : String("-"), C_GREEN);
  infoCard(164, BODY_Y + 61, 152, 34, "DNS", dns, C_BLUE);
  infoCard(4, BODY_Y + 100, 210, 34, "DHCP SERVER / RELAY", srv, C_ORANGE);
  infoCard(222, BODY_Y + 100, 94, 34, "LEASE", lease, C_PINK);

  g->fillRoundRect(4, BODY_Y + 139, 312, 23, 6, C_CARD);
  text("SWITCH", 10, BODY_Y + 150, C_PURPLE, &fonts::Font2, textdatum_t::middle_left);
  String sw = haveNb ? String(nb.device) + "  " + shortPort(nb.port)
                     : String(st.link ? "listening for CDP / LLDP" : "not connected");
  text(sw, 65, BODY_Y + 150, haveNb ? C_TEXT : C_DIM, &fonts::Font2,
       textdatum_t::middle_left, 244);
}

static void drawSwitchPage() {
  if (!haveNb) {
    g->fillRoundRect(4, BODY_Y, 312, 160, 8, C_CARD);
    if (!st.link) {
      text(lanOk ? "Plug into a switch port" : "LAN module not found", 160, BODY_Y + 70, C_RED,
           &fonts::FreeSansBold12pt7b, textdatum_t::middle_center);
      return;
    }
    uint32_t secs = (millis() - st.linkUpMs) / 1000;
    text("Listening for CDP / LLDP", 160, BODY_Y + 34, C_YELLOW, &fonts::FreeSansBold12pt7b,
         textdatum_t::middle_center);
    text(String(secs) + " s", 160, BODY_Y + 70, C_TEXT, &fonts::FreeSansBold18pt7b, textdatum_t::middle_center);
    int w = (int)min<uint32_t>(secs, 60) * 280 / 60;
    g->fillRoundRect(20, BODY_Y + 96, 280, 10, 5, C_BG);
    if (w > 6) g->fillRoundRect(20, BODY_Y + 96, w, 10, 5, secs > 60 ? C_ORANGE : C_CYAN);
    text(secs > 65 ? "Nothing heard - CDP/LLDP may be disabled here"
                   : "Switches announce every 30-60 seconds",
         160, BODY_Y + 122, C_DIM, &fonts::Font2, textdatum_t::middle_center);
    text(String(st.rxFrames) + " frames seen" + (st.vlanTagCount ? "   " + vlanSummary() : String("")),
         160, BODY_Y + 142, C_DIM, &fonts::Font2, textdatum_t::middle_center);
    return;
  }

  // Headline: switch name + port, with a big VLAN badge
  g->fillRoundRect(4, BODY_Y, 312, 62, 8, C_CARD);
  text(nb.device, 12, BODY_Y + 16, C_TEXT, &fonts::FreeSansBold9pt7b, textdatum_t::middle_left, 192);
  text(shortPort(nb.port), 12, BODY_Y + 43, C_YELLOW, &fonts::FreeSansBold12pt7b, textdatum_t::middle_left, 192);
  vlanTile(210, BODY_Y + 3, 102, 56, &fonts::FreeSansBold18pt7b);

  int y = BODY_Y + 67;
  const int dy = 19;
  String vl = vlanSummary();
  row(y, "VLANS", vl.length() ? vl : String("not announced"), C_YELLOW); y += dy;
  row(y, "MGMT IP", nb.mgmtIp[0] ? String(nb.mgmtIp) : String("-"), C_CYAN); y += dy;
  row(y, "MODEL", nb.platform[0] ? String(nb.platform) : String("-"), C_PINK); y += dy;
  if (nb.software[0]) row(y, "VERSION", nb.software, C_BLUE);
  else row(y, "PORT", nb.portDesc[0] ? String(nb.portDesc) : String(nb.port), C_BLUE);
  y += dy;

  bool mismatch = nb.duplex == 1 && !st.fullDuplex && st.link;
  String info = String(nbProto) + "  " + String((millis() - nb.lastSeenMs) / 1000) + "s ago";
  if (mismatch) info = "DUPLEX MISMATCH (switch full, us half)";
  else {
    if (nb.duplex >= 0) info += nb.duplex ? "  full-dup" : "  half-dup";
    if (nb.poeMw > 0) info += "  PoE " + String(nb.poeMw / 1000.0f, 1) + "W";
    if (nb.caps[0]) info += String("  ") + nb.caps;
  }
  row(y, "SOURCE", info, C_PURPLE, mismatch ? C_RED : C_TEXT);
}

static void toolRect(int i, int &x, int &y, int &w, int &h) {
  // Restore the compact eight-tool grid; each large, full tile is a touch target.
  w = 152; h = 35;
  x = 6 + (i % 2) * 156;
  y = 34 + (i / 2) * 39;
}

static void finderPattern(int x, int y, uint16_t color) {
  g->drawRect(x, y, 11, 11, color);
  g->fillRect(x + 3, y + 3, 5, 5, color);
}

static void drawToolIcon(ToolId id, int cx, int cy, uint16_t color) {
  // Simple vector illustrations stay crisp on the Core2's 320 x 240 display.
  switch (id) {
    case TOOL_PING_GW: // gateway/router
      g->drawRoundRect(cx - 13, cy - 3, 26, 15, 3, color);
      g->drawLine(cx - 7, cy - 3, cx - 10, cy - 10, color);
      g->drawLine(cx + 7, cy - 3, cx + 10, cy - 10, color);
      g->fillCircle(cx - 7, cy + 4, 1, color);
      g->fillCircle(cx, cy + 4, 1, color);
      g->fillCircle(cx + 7, cy + 4, 1, color);
      break;
    case TOOL_PING_INET: // internet globe
      g->drawCircle(cx, cy, 12, color);
      g->drawEllipse(cx, cy, 5, 12, color);
      g->drawEllipse(cx, cy, 12, 5, color);
      break;
    case TOOL_TRACE: // route with hops
      g->drawLine(cx - 12, cy + 7, cx - 3, cy - 6, color);
      g->drawLine(cx - 3, cy - 6, cx + 4, cy + 5, color);
      g->drawLine(cx + 4, cy + 5, cx + 12, cy - 7, color);
      g->fillCircle(cx - 12, cy + 7, 3, color);
      g->fillCircle(cx - 3, cy - 6, 3, color);
      g->fillCircle(cx + 4, cy + 5, 3, color);
      g->fillCircle(cx + 12, cy - 7, 3, color);
      break;
    case TOOL_DNS_WEB: // browser window and web globe
      g->drawRoundRect(cx - 14, cy - 10, 28, 21, 3, color);
      g->drawLine(cx - 13, cy - 4, cx + 13, cy - 4, color);
      g->fillCircle(cx - 9, cy - 7, 1, color);
      g->fillCircle(cx - 5, cy - 7, 1, color);
      g->drawCircle(cx + 3, cy + 3, 6, color);
      g->drawEllipse(cx + 3, cy + 3, 2, 6, color);
      g->drawEllipse(cx + 3, cy + 3, 6, 2, color);
      break;
    case TOOL_DHCP_SCAN: // stacked server
      for (int j = 0; j < 2; j++) {
        int yy = cy - 10 + j * 12;
        g->drawRoundRect(cx - 13, yy, 26, 10, 2, color);
        g->fillCircle(cx - 8, yy + 5, 1, color);
        g->drawLine(cx - 3, yy + 5, cx + 9, yy + 5, color);
      }
      break;
    case TOOL_BLINK_PORT: // switch ports and activity spark
      g->drawRoundRect(cx - 14, cy - 8, 28, 17, 3, color);
      for (int j = 0; j < 4; j++) g->drawRect(cx - 10 + j * 6, cy - 3, 4, 6, color);
      g->drawLine(cx + 9, cy - 12, cx + 12, cy - 8, color);
      g->drawLine(cx + 8, cy - 7, cx + 14, cy - 7, color);
      break;
    case TOOL_QR: // QR tile with three finder corners
      finderPattern(cx - 12, cy - 11, color);
      finderPattern(cx + 2, cy - 11, color);
      finderPattern(cx - 12, cy + 3, color);
      g->fillRect(cx + 3, cy + 4, 3, 3, color);
      g->fillRect(cx + 9, cy + 4, 3, 3, color);
      g->fillRect(cx + 3, cy + 10, 3, 3, color);
      g->fillRect(cx + 9, cy + 10, 3, 3, color);
      break;
    case TOOL_EXTRA: // four app tiles with a plus: opens the extra tools menu
      for (int j = 0; j < 3; ++j)
        g->drawRoundRect(cx - 12 + (j % 2) * 14, cy - 12 + (j / 2) * 14, 10, 10, 2, color);
      g->drawLine(cx + 7, cy + 2, cx + 7, cy + 12, color);
      g->drawLine(cx + 2, cy + 7, cx + 12, cy + 7, color);
      break;
    default:
      g->drawCircle(cx, cy, 10, color);
      break;
  }
}

static void drawToolsPage() {
  for (int i = 0; i < 8; i++) {
    int x, y, w, h;
    toolRect(i, x, y, w, h);
    const ToolButton &b = TOOL_BUTTONS[i];
    bool pressed = pressedTool == i;
    g->fillRoundRect(x, y, w, h, 7, pressed ? b.color : C_CARD);
    g->drawRoundRect(x, y, w, h, 7, b.color);
    drawToolIcon(b.id, x + w / 2, y + h / 2, pressed ? rgb(0, 0, 0) : b.color);
  }
}

// ---------------------------------------------------------------- overlays
static const int CON_ROWS = 9;

static void drawConsole() {
  static ConLine lines[CON_MAX_LINES];
  int n = conCopy(lines, CON_MAX_LINES);
  bool busy = toolBusy();
  String title = conTitle();
  if (busy) title += String("  ") + "|/-\\"[(millis() / 200) % 4];
  text(title, 8, BODY_Y + 9, C_CYAN, &fonts::FreeSansBold9pt7b, textdatum_t::middle_left);

  int maxScroll = max(0, n - CON_ROWS);
  if (conScroll > maxScroll) conScroll = maxScroll;
  int first = max(0, n - CON_ROWS - conScroll);
  int y = BODY_Y + 20;
  for (int i = first; i < n && i < first + CON_ROWS; i++, y += 16)
    text(lines[i].text, 8, y, lines[i].color, &fonts::Font2, textdatum_t::top_left, SCREEN_W - 12);

  bigButton(3, TAB_Y + 2, 150, TAB_H - 4, busy ? C_RED : C_GREEN, true, busy ? "STOP" : "BACK");
  bigButton(159, TAB_Y + 2, 76, TAB_H - 4, C_CYAN, false, "UP");
  bigButton(241, TAB_Y + 2, 76, TAB_H - 4, C_CYAN, false, "DOWN");
}

static void drawQr() {
  String q;
  if (haveNb) {
    q += String("SW: ") + nb.device + "\nPORT: " + nb.port + "\n";
    if (nb.nativeVlan >= 0) q += "VLAN: " + String(nb.nativeVlan) + "\n";
    if (nb.voiceVlan >= 0) q += "VOICE: " + String(nb.voiceVlan) + "\n";
    if (nb.mgmtIp[0]) q += String("MGMT: ") + nb.mgmtIp + "\n";
    if (nb.platform[0]) q += String("MODEL: ") + nb.platform + "\n";
  }
  if (st.hasIp) {
    q += "IP: " + ipToStr(st.ip) + "/" + String(maskToPrefix(st.mask)) + "\nGW: " + ipToStr(st.gw) + "\n";
    if (dhcp.valid) q += "DHCP: " + ipToStr(dhcp.server) + "\n";
  }
  q += "MAC: " + macToStr(st.mac);

  g->fillRoundRect(4, BODY_Y, 164, 164, 8, C_TEXT);
  g->qrcode(q.c_str(), 10, BODY_Y + 6, 152, 1);
  text("SCAN ME", 176, BODY_Y + 4, C_CYAN, &fonts::FreeSansBold9pt7b);
  text("Port details as", 176, BODY_Y + 28, C_DIM, &fonts::Font2);
  text("text for your phone", 176, BODY_Y + 44, C_DIM, &fonts::Font2);
  if (haveNb) {
    text(nb.device, 176, BODY_Y + 72, C_TEXT, &fonts::Font2, textdatum_t::top_left, 140);
    text(shortPort(nb.port), 176, BODY_Y + 90, C_YELLOW, &fonts::FreeSansBold9pt7b, textdatum_t::top_left, 140);
    if (nb.nativeVlan >= 0)
      text("VLAN " + String(nb.nativeVlan), 176, BODY_Y + 114, C_GREEN, &fonts::FreeSansBold9pt7b);
  }
  if (st.hasIp) text(ipToStr(st.ip), 176, BODY_Y + 140, C_CYAN, &fonts::Font2);
  bigButton(3, TAB_Y + 2, 314, TAB_H - 4, C_GREEN, true, "BACK");
}

// ---------------------------------------------------------------- wireless survey
static uint16_t signalColor(int rssi) {
  return rssi >= -67 ? C_GREEN : rssi >= -75 ? C_YELLOW : C_ORANGE;
}

static void signalBars(int x, int bottom, int rssi) {
  int lit = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
  for (int i = 0; i < 4; ++i)
    g->fillRoundRect(x + i * 5, bottom - 4 - i * 4, 3, 4 + i * 4, 1,
                     i < lit ? signalColor(rssi) : rgb(47, 62, 84));
}

static void wifiIcon(int cx, int cy, uint16_t color) {
  // Three cropped concentric arcs, drawn as short segments for a crisp radio icon.
  for (int r = 8; r <= 20; r += 6) {
    int prevX = cx - r * 7 / 10, prevY = cy - r * 7 / 10;
    for (int dx = -r * 7 / 10 + 1; dx <= r * 7 / 10; ++dx) {
      int yy = cy - int(sqrtf(float(r * r - dx * dx)));
      g->drawLine(prevX, prevY, cx + dx, yy, color);
      prevX = cx + dx; prevY = yy;
    }
  }
  g->fillCircle(cx, cy - 1, 2, color);
}

static void drawExtraTools() {
  text("EXTRA TOOLS", 12, 41, C_TEXT, &fonts::FreeSansBold12pt7b);
  text("Wireless discovery & diagnostics", 12, 68, C_DIM, &fonts::Font2);
  g->fillRoundRect(6, 94, 308, 78, 12, C_CARD);
  g->drawRoundRect(6, 94, 308, 78, 12, rgb(43, 71, 99));
  g->fillRoundRect(16, 108, 50, 50, 10, rgb(15, 58, 73));
  wifiIcon(41, 146, C_CYAN);
  text("Wi-Fi scanner", 80, 111, C_TEXT, &fonts::FreeSansBold12pt7b,
       textdatum_t::top_left, 216);
  text("SSIDs, signal & AP details", 80, 141, C_DIM, &fonts::Font2);
  text(">", 298, 129, C_CYAN, &fonts::Font2, textdatum_t::middle_center);
  text("2.4 GHz  /  No connection required", 160, 185, C_DIM, &fonts::Font0,
       textdatum_t::middle_center);
  bigButton(3, TAB_Y + 2, 314, TAB_H - 4, C_CYAN, false, "BACK TO TOOLS");
}

static void drawWifiHeader() {
  g->fillRect(0, 0, SCREEN_W, BAR_H, rgb(15, 27, 44));
  g->fillRect(0, BAR_H - 1, SCREEN_W, 1, rgb(37, 61, 81));
  g->fillCircle(12, 13, 3, wifiScannerState() == WIFI_SURVEY_SCANNING ? C_YELLOW : C_CYAN);
  text("WI-FI SURVEY", 23, 14, C_TEXT, &fonts::FreeSansBold9pt7b, textdatum_t::middle_left);
  g->fillRoundRect(250, 5, 62, 18, 5, rgb(15, 58, 73));
  text("2.4 GHz", 281, 14, C_CYAN, &fonts::Font0, textdatum_t::middle_center);
}

static int wifiPageCount() { return max(1, (wifiScannerCount() + WIFI_ROWS - 1) / WIFI_ROWS); }

static void drawWifiList() {
  WifiScanState state = wifiScannerState();
  bool scanning = state == WIFI_SURVEY_SCANNING;
  String heading = scanning ? "Discovering networks" : state == WIFI_SURVEY_FAILED ? "Scan unavailable"
                   : String(wifiScannerCount()) + " access points";
  text(heading, 9, 41, C_TEXT, &fonts::FreeSansBold9pt7b, textdatum_t::middle_left, 235);
  g->fillRoundRect(253, 32, 61, 27, 6, scanning ? C_CARD : rgb(15, 58, 73));
  text(scanning ? "WAIT" : "RESCAN", 283, 45, scanning ? C_DIM : C_CYAN,
       &fonts::Font0, textdatum_t::middle_center);
  String subtitle = scanning ? "Listening across available channels..." : state == WIFI_SURVEY_FAILED ? "Tap RESCAN to try again"
    : String("Strongest first  |  ") + wifiScannerAgeSeconds() + "s ago  |  " + (wifiListPage + 1) + "/" + wifiPageCount();
  text(subtitle, 9, 62, C_DIM, &fonts::Font0, textdatum_t::middle_left, 302);

  if (scanning || state == WIFI_SURVEY_FAILED || wifiScannerCount() == 0) {
    g->fillRoundRect(6, 74, 308, 117, 10, C_CARD);
    wifiIcon(160, 119, state == WIFI_SURVEY_FAILED ? C_ORANGE : C_CYAN);
    text(scanning ? "Scanning 2.4 GHz" : state == WIFI_SURVEY_FAILED ? "Could not scan" : "No access points found",
         160, 138, C_TEXT, &fonts::FreeSansBold9pt7b, textdatum_t::middle_center);
    text(scanning ? "You can go back while the scan runs." : state == WIFI_SURVEY_FAILED ? wifiScannerError()
              : "Move closer to an AP, then tap RESCAN.",
         160, 160, C_DIM, &fonts::Font2, textdatum_t::middle_center, 288);
    if (scanning) {
      g->fillRoundRect(32, 177, 256, 3, 1, C_BG);
      int offset = (millis() / 18) % 209;
      g->fillRoundRect(32 + offset, 177, 48, 3, 1, C_CYAN);
    } else text("5 / 6 GHz are outside this radio's range", 160, 180, C_DIM, &fonts::Font0,
                textdatum_t::middle_center);
  } else {
    for (int i = 0; i < WIFI_ROWS; ++i) {
      const wifi_ap_record_t *ap = wifiScannerAp(wifiListPage * WIFI_ROWS + i);
      if (!ap) break;
      int y = 72 + i * 31;
      g->fillRoundRect(6, y, 308, 29, 6, C_CARD);
      signalBars(14, y + 23, ap->rssi);
      text(wifiSsid(*ap), 42, y + 9, C_TEXT, &fonts::FreeSansBold9pt7b,
           textdatum_t::middle_left, 185);
      text(String(ap->rssi) + " dBm", 292, y + 9, signalColor(ap->rssi), &fonts::Font2,
           textdatum_t::middle_right);
      String meta = "CH " + String(ap->primary) + "  " + wifiAuthName(ap->authmode)
                    + "  " + macToStr(ap->bssid).substring(9);
      text(meta, 42, y + 23, C_DIM, &fonts::Font0, textdatum_t::middle_left, 252);
      text(">", 305, y + 15, C_DIM, &fonts::Font2, textdatum_t::middle_center);
    }
  }
  bigButton(3, TAB_Y + 2, 102, TAB_H - 4, C_CYAN, false, "BACK");
  bigButton(109, TAB_Y + 2, 102, TAB_H - 4, wifiListPage > 0 ? C_CYAN : C_DIM, false, "PREV");
  bigButton(215, TAB_Y + 2, 102, TAB_H - 4,
            wifiListPage + 1 < wifiPageCount() ? C_CYAN : C_DIM, false, "NEXT");
}

static void wifiDetailRow(int y, const char *label, const String &value, uint16_t color = C_TEXT) {
  text(label, 12, y, C_DIM, &fonts::Font2, textdatum_t::middle_left);
  text(value, 111, y, color, &fonts::Font2, textdatum_t::middle_left, 198);
}

static void drawWifiDetail() {
  const wifi_ap_record_t *ap = wifiScannerAp(wifiSelected);
  if (!ap) return; // selection is cleared before every rescan / scanner exit
  g->fillRoundRect(6, 33, 308, 43, 8, C_CARD);
  text("NETWORK NAME / SSID", 14, 41, C_CYAN, &fonts::Font0, textdatum_t::middle_left);
  // A 32-byte SSID gets a complete two-line view on the identity page below.
  text(wifiSsid(*ap), 14, 60, C_TEXT, &fonts::FreeSansBold9pt7b,
       textdatum_t::middle_left, 290);

  if (wifiDetailPage == 0) {
    String ssid = ap->ssid[0] ? wifiSsid(*ap) : String("Hidden (not advertised)");
    g->setFont(&fonts::Font2);
    int split = ssid.length();
    while (split > 0 && g->textWidth(ssid.substring(0, split)) > 286) --split;
    text(ssid.substring(0, split), 14, 83, C_TEXT, &fonts::Font2, textdatum_t::top_left);
    text(ssid.substring(split), 14, 99, C_TEXT, &fonts::Font2, textdatum_t::top_left, 286);
    wifiDetailRow(124, "BSSID / MAC", macToStr(ap->bssid), C_CYAN);
    wifiDetailRow(145, "Security", wifiAuthName(ap->authmode));
    wifiDetailRow(166, "AP hostname", "Not in scan data");
    text("SSID is the advertised name; IP/model need AP access.", 160, 188, C_DIM,
         &fonts::Font0, textdatum_t::middle_center);
  } else {
    uint16_t accent = signalColor(ap->rssi);
    g->fillRoundRect(6, 81, 116, 79, 8, C_CARD);
    text(String(ap->rssi), 14, 88, accent, &fonts::FreeSansBold18pt7b);
    text("dBm", 113, 111, C_DIM, &fonts::Font0, textdatum_t::bottom_right);
    text(wifiSignalName(ap->rssi), 14, 127, accent, &fonts::Font2);
    g->fillRoundRect(14, 150, 100, 3, 1, C_BG);
    int width = wifiSignalPercent(ap->rssi);
    if (width) g->fillRoundRect(14, 150, width, 3, 1, accent);
    int secondary = ap->second == WIFI_SECOND_CHAN_ABOVE ? ap->primary + 4
                  : ap->second == WIFI_SECOND_CHAN_BELOW ? ap->primary - 4 : 0;
    text("CHANNEL " + String(ap->primary) + "  /  " + wifiChannelMHz(ap->primary) + " MHz",
         132, 87, C_CYAN, &fonts::Font2, textdatum_t::top_left, 176);
    text(secondary ? "40 MHz  |  2nd CH " + String(secondary) : String("20 MHz  |  no 2nd channel"),
         132, 107, C_TEXT, &fonts::Font0, textdatum_t::top_left, 176);
    text(wifiPhyName(*ap), 132, 122, C_TEXT, &fonts::Font2, textdatum_t::top_left, 176);
    String country = "Not advertised";
    if (ap->country.cc[0] >= 'A' && ap->country.cc[0] <= 'Z' && ap->country.cc[1] >= 'A' && ap->country.cc[1] <= 'Z')
      country = String(ap->country.cc[0]) + ap->country.cc[1];
    text("CC: " + (country == "Not advertised" ? String("--") : country) + "  |  WPS: " + (ap->wps ? "Yes" : "No"),
         132, 145, C_DIM, &fonts::Font0, textdatum_t::top_left, 176);
    text("Pair: " + String(wifiCipherName(ap->pairwise_cipher)) + "   Group: " + wifiCipherName(ap->group_cipher),
         12, 171, C_TEXT, &fonts::Font0, textdatum_t::middle_left, 297);
    text(String("Scan snapshot  /  ") + wifiScannerAgeSeconds() + "s ago  /  BACK > RESCAN to update",
         160, 188, C_DIM, &fonts::Font0, textdatum_t::middle_center);
  }
  bigButton(3, TAB_Y + 2, 102, TAB_H - 4, C_CYAN, false, "BACK");
  bigButton(109, TAB_Y + 2, 102, TAB_H - 4, C_CYAN, wifiDetailPage == 0, "IDENTITY");
  bigButton(215, TAB_Y + 2, 102, TAB_H - 4, C_CYAN, wifiDetailPage == 1, "RADIO");
}

static void render() {
  if (useCanvas) g = &canvas;
  else netBusLock();                          // drawing straight to the panel
  g->fillScreen(C_BG);
  if (overlay == OV_WIFI_LIST || overlay == OV_WIFI_DETAIL) drawWifiHeader();
  else drawStatusBar();
  if (overlay == OV_CONSOLE) drawConsole();
  else if (overlay == OV_QR) drawQr();
  else if (overlay == OV_EXTRA) drawExtraTools();
  else if (overlay == OV_WIFI_LIST) drawWifiList();
  else if (overlay == OV_WIFI_DETAIL) drawWifiDetail();
  else {
    if (page == PAGE_NET) drawNetPage();
    else if (page == PAGE_SWITCH) drawSwitchPage();
    else drawToolsPage();
    drawTabs();
  }
  if (!useCanvas) netBusUnlock();
  if (useCanvas) {
    // Push in strips so the Ethernet driver never waits long for the shared SPI bus
    auto *buf = (lgfx::swap565_t *)canvas.getBuffer();
    const int strip = 40;
    for (int y = 0; y < SCREEN_H; y += strip) {
      netBusLock();
      M5.Display.pushImage(0, y, SCREEN_W, strip, buf + y * SCREEN_W);
      netBusUnlock();
    }
  }
}

// ---------------------------------------------------------------- input
// NO_SOUND builds never start the speaker (on the LAN 13.2 module the W5500 reset
// line shares GPIO0 with the Core2 speaker clock).
static void beep(int freq, int ms, bool queue = false) {
#ifndef NO_SOUND
  M5.Speaker.tone(freq, ms, -1, !queue);
#endif
}
static void click() {
  beep(2200, 25);
  M5.Power.setVibration(90);
  vibrateOffAt = millis() + 35;
}

static void closeOverlay() {
  if (overlay == OV_CONSOLE && toolBusy()) { toolStop(); return; }   // first tap stops, second goes back
  overlay = OV_NONE;
}

static void bottomButton(int idx) {           // 0..2: the three zones under / at the bottom of the screen
  click();
  if (overlay == OV_EXTRA) overlay = OV_NONE;
  else if (overlay == OV_WIFI_LIST) {
    if (idx == 0) {
      wifiScannerClose();
      wifiSelected = -1;
      overlay = OV_EXTRA;
    } else if (idx == 1) wifiListPage = max(0, wifiListPage - 1);
    else wifiListPage = min(wifiPageCount() - 1, wifiListPage + 1);
  } else if (overlay == OV_WIFI_DETAIL) {
    if (idx == 0) overlay = OV_WIFI_LIST;
    else wifiDetailPage = idx - 1;
  } else if (overlay == OV_QR) closeOverlay();
  else if (overlay == OV_CONSOLE) {
    if (idx == 0) closeOverlay();
    else if (idx == 1) conScroll += 4;
    else conScroll = max(0, conScroll - 4);
  } else {
    page = (Page)idx;
  }
  dirty = true;
}

static void handleTouch(int x, int y) {
  if (y >= TAB_Y) {
    if (overlay == OV_CONSOLE) bottomButton(x < 156 ? 0 : x < 238 ? 1 : 2);
    else if (overlay == OV_WIFI_LIST || overlay == OV_WIFI_DETAIL) bottomButton(x < 107 ? 0 : x < 213 ? 1 : 2);
    else bottomButton(min(2, x / 107));
    return;
  }
  if (overlay == OV_EXTRA) {
    if (x >= 6 && x < 314 && y >= 94 && y < 172) {
      click();
      wifiListPage = 0;
      wifiSelected = -1;
      overlay = OV_WIFI_LIST;
      wifiScannerOpen();
      dirty = true;
    }
    return;
  }
  if (overlay == OV_WIFI_LIST) {
    if (x >= 253 && x < 314 && y >= 32 && y < 59 && wifiScannerState() != WIFI_SURVEY_SCANNING) {
      click();
      wifiSelected = -1;
      wifiListPage = 0;
      wifiScannerRefresh();
      dirty = true;
    } else if (wifiScannerState() == WIFI_SURVEY_READY && x >= 6 && x < 314 && y >= 72 && y < 194) {
      int rowIndex = (y - 72) / 31;
      int index = wifiListPage * WIFI_ROWS + rowIndex;
      if ((y - 72) % 31 < 29 && rowIndex < WIFI_ROWS && wifiScannerAp(index)) {
        click();
        wifiSelected = index;
        wifiDetailPage = 1;
        overlay = OV_WIFI_DETAIL;
        dirty = true;
      }
    }
    return;
  }
  if (overlay != OV_NONE || page != PAGE_TOOLS) return;
  for (int i = 0; i < 8; i++) {
    int bx, by, bw, bh;
    toolRect(i, bx, by, bw, bh);
    if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
      click();
      pressedTool = i;
      render();
      delay(45);
      pressedTool = -1;
      const ToolButton &selected = TOOL_BUTTONS[i];
      if (selected.id == TOOL_QR) overlay = OV_QR;
      else if (selected.id == TOOL_EXTRA) overlay = OV_EXTRA;
      else {
        conScroll = 0;
        overlay = OV_CONSOLE;
        toolStart(selected.id);
      }
      dirty = true;
      return;
    }
  }
}

// ---------------------------------------------------------------- data polling
static void poll() {
  static bool prevLink = false, prevNb = false, prevIp = false;
  NetState ns;
  netGetState(ns);
  Neighbor n;
  const char *proto = "";
  bool have = netBestNeighbor(n, proto);
  DhcpInfo d;
  netGetDhcp(d);

  if (ns.link != prevLink) {
    beep(ns.link ? 1800 : 500, ns.link ? 80 : 200);
    prevLink = ns.link;
  }
  if (have && !prevNb) {                      // switch identified: buzz + two-tone
    beep(2000, 70);
    beep(2800, 120, true);
    M5.Power.setVibration(160);
    vibrateOffAt = millis() + 200;
  }
  prevNb = have;
  if (ns.hasIp && !prevIp) beep(2400, 60);
  prevIp = ns.hasIp;

  st = ns; nb = n; nbProto = proto; haveNb = have; dhcp = d;
}

// ---------------------------------------------------------------- setup / loop
static void splash(const char *msg) {
  netBusLock();
  M5.Display.fillScreen(C_BG);
  static const uint16_t stripes[6] = {C_CYAN, C_GREEN, C_YELLOW, C_ORANGE, C_PINK, C_PURPLE};
  for (int i = 0; i < 6; i++) M5.Display.fillRect(i * 54, 0, 54, 10, stripes[i]);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.setTextColor(C_TEXT);
  M5.Display.drawString("NET TOOL", 160, 100);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(C_DIM);
  M5.Display.drawString(msg, 160, 150);
  netBusUnlock();
}

void setup() {
  auto cfg = M5.config();
  cfg.output_power = true;                    // feed 5V to the stacked LAN module from the battery
  cfg.internal_mic = false;                   // network tool does not use audio input
#ifdef NO_SOUND
  cfg.internal_spk = false;                   // GPIO0 belongs to the W5500 reset, not I2S
#endif
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.printf("board=%d default_rot=%d w=%d h=%d\n", (int)M5.getBoard(), (int)M5.Display.getRotation(),
                (int)M5.Display.width(), (int)M5.Display.height());
  M5.Display.setBrightness(180);
#ifndef NO_SOUND
  M5.Speaker.setVolume(110);
#endif
  splash("starting Ethernet...");

  toolsBegin();
  lanOk = netBegin();
  if (!lanOk) {
    splash("W5500 LAN module not detected");
    delay(1500);
  } else {
    splash(netModuleName());
    delay(800);
  }

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  useCanvas = canvas.createSprite(SCREEN_W, SCREEN_H) != nullptr;
  poll();
}

void loop() {
  static uint32_t lastPoll = 0, lastDraw = 0;
  M5.update();
  if (wifiScannerUpdate()) dirty = true;

  auto t = M5.Touch.getDetail();
  if (t.wasPressed() && t.y < SCREEN_H) handleTouch(t.x, t.y);
  if (M5.BtnA.wasPressed()) bottomButton(0);  // touch zones printed below the screen
  if (M5.BtnB.wasPressed()) bottomButton(1);
  if (M5.BtnC.wasPressed()) bottomButton(2);

  uint32_t now = millis();
  if (vibrateOffAt && now >= vibrateOffAt) {
    M5.Power.setVibration(0);
    vibrateOffAt = 0;
  }
  if (now - lastPoll >= 250) {
    lastPoll = now;
    poll();
    if (conVersion() != lastConVersion) {
      lastConVersion = conVersion();
      dirty = true;
    }
  }
  // Redraw on change, and twice a second for the timers / blinking link dot
  if ((dirty && now - lastDraw >= 60) || now - lastDraw >= 500) {
    lastDraw = now;
    dirty = false;
    render();
  }
  delay(10);
}
