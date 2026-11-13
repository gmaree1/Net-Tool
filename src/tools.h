// Troubleshooting tools. Each runs in its own task and writes to a shared console
// that the UI renders.
#pragma once
#include <Arduino.h>

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// Palette
constexpr uint16_t C_BG     = rgb(8, 14, 28);
constexpr uint16_t C_CARD   = rgb(24, 36, 62);
constexpr uint16_t C_TEXT   = rgb(255, 255, 255);
constexpr uint16_t C_DIM    = rgb(140, 160, 195);
constexpr uint16_t C_CYAN   = rgb(34, 211, 238);
constexpr uint16_t C_GREEN  = rgb(34, 197, 94);
constexpr uint16_t C_YELLOW = rgb(250, 204, 21);
constexpr uint16_t C_ORANGE = rgb(249, 115, 22);
constexpr uint16_t C_RED    = rgb(239, 68, 68);
constexpr uint16_t C_PURPLE = rgb(168, 85, 247);
constexpr uint16_t C_BLUE   = rgb(59, 130, 246);
constexpr uint16_t C_PINK   = rgb(236, 72, 153);
constexpr uint16_t C_TEAL   = rgb(20, 184, 166);

enum ToolId {
  TOOL_PING_GW, TOOL_PING_INET, TOOL_TRACE, TOOL_DNS_WEB,
  TOOL_DHCP_SCAN, TOOL_BLINK_PORT, TOOL_QR, TOOL_EXTRA,
  TOOL_COUNT
};

struct ConLine {
  char     text[48];
  uint16_t color;
};
#define CON_MAX_LINES 64

void        toolStart(ToolId id);       // clears the console and launches the tool
bool        toolBusy();
void        toolStop();                 // ask the running tool to finish early
const char *conTitle();
uint32_t    conVersion();               // changes whenever the console content changes
int         conCopy(ConLine *out, int max);   // oldest first; returns count
void        toolsBegin();               // initializes the console and test tools
