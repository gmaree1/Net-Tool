// Background Ethernet diagnostics: ping, ICMP traceroute, DNS / HTTP checks,
// DHCP offer collection, and switch-port blinking through PHY link cycling.
#include "tools.h"
#include "net.h"
#include "power.h"
#include <stdarg.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "ping/ping_sock.h"

static const uint8_t INET_TARGET[4] = {8, 8, 8, 8};

// ---------------------------------------------------------------- console
static SemaphoreHandle_t sConLock;
static ConLine  sLines[CON_MAX_LINES];
static int      sLineCount = 0;
static uint32_t sConVersion = 0;
static char     sTitle[24] = "";
static volatile bool sBusy = false;
static volatile bool sAbort = false;

static void conPrint(uint16_t color, const char *fmt, ...) {
  char buf[sizeof(ConLine::text)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  xSemaphoreTake(sConLock, portMAX_DELAY);
  if (sLineCount == CON_MAX_LINES) {
    memmove(&sLines[0], &sLines[1], sizeof(ConLine) * (CON_MAX_LINES - 1));
    sLineCount--;
  }
  strlcpy(sLines[sLineCount].text, buf, sizeof(ConLine::text));
  sLines[sLineCount].color = color;
  sLineCount++;
  sConVersion++;
  xSemaphoreGive(sConLock);
}

static void conReset(const char *title) {
  xSemaphoreTake(sConLock, portMAX_DELAY);
  sLineCount = 0;
  strlcpy(sTitle, title, sizeof sTitle);
  sConVersion++;
  xSemaphoreGive(sConLock);
}

const char *conTitle() { return sTitle; }
uint32_t conVersion() { return sConVersion; }
int conCopy(ConLine *out, int max) {
  xSemaphoreTake(sConLock, portMAX_DELAY);
  int n = sLineCount < max ? sLineCount : max;
  memcpy(out, &sLines[sLineCount - n], sizeof(ConLine) * n);
  xSemaphoreGive(sConLock);
  return n;
}
bool toolBusy() { return sBusy; }
void toolStop() { sAbort = true; }

// Sleep in small steps so STOP is responsive. Returns false if aborted.
static bool waitMs(uint32_t ms) {
  uint32_t end = millis() + ms;
  while ((int32_t)(end - millis()) > 0) {
    if (sAbort) return false;
    delay(50);
  }
  return !sAbort;
}

static bool needIp(NetState &st) {
  // Cable-sleep may have just been woken by toolStart; give the PHY a moment.
  for (int i = 0; i < 60; i++) {
    netGetState(st);
    if (st.link) break;
    if (!waitMs(50)) return false;
  }
  if (!st.link) { conPrint(C_RED, "No link - plug in a cable"); return false; }
  if (!st.hasIp) { conPrint(C_RED, "No IP address yet (DHCP pending)"); return false; }
  return true;
}

// ---------------------------------------------------------------- ping
static SemaphoreHandle_t sPingDone;
static uint32_t sPingMin, sPingMax, sPingSum, sPingOk;

static void onPingOk(esp_ping_handle_t h, void *) {
  uint16_t seq = 0; uint8_t ttl = 0; uint32_t ms = 0;
  esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof seq);
  esp_ping_get_profile(h, ESP_PING_PROF_TTL, &ttl, sizeof ttl);
  esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof ms);
  if (ms < sPingMin) sPingMin = ms;
  if (ms > sPingMax) sPingMax = ms;
  sPingSum += ms; sPingOk++;
  conPrint(C_GREEN, "#%u  reply   %lu ms   ttl %u", seq, (unsigned long)ms, ttl);
}
static void onPingTimeout(esp_ping_handle_t h, void *) {
  uint16_t seq = 0;
  esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof seq);
  conPrint(C_RED, "#%u  timeout", seq);
}
static void onPingEnd(esp_ping_handle_t, void *) { xSemaphoreGive(sPingDone); }

static void doPing(const uint8_t ip[4], int count) {
  conPrint(C_CYAN, "PING %s", ipToStr(ip).c_str());
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  IP_ADDR4(&cfg.target_addr, ip[0], ip[1], ip[2], ip[3]);
  cfg.count = count;
  cfg.interval_ms = 500;
  cfg.timeout_ms = 1000;
  cfg.task_stack_size = 4096;
  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = onPingOk;
  cbs.on_ping_timeout = onPingTimeout;
  cbs.on_ping_end = onPingEnd;
  sPingMin = UINT32_MAX; sPingMax = 0; sPingSum = 0; sPingOk = 0;
  xSemaphoreTake(sPingDone, 0);
  esp_ping_handle_t h = nullptr;
  if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
    conPrint(C_RED, "Could not start ping");
    return;
  }
  esp_ping_start(h);
  bool finished = false;
  while (!finished && !sAbort) finished = xSemaphoreTake(sPingDone, pdMS_TO_TICKS(100)) == pdTRUE;
  uint32_t sent = 0;
  esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof sent);
  esp_ping_stop(h);
  esp_ping_delete_session(h);
  uint32_t loss = sent ? (sent - sPingOk) * 100 / sent : 100;
  conPrint(loss == 0 ? C_GREEN : (sPingOk ? C_YELLOW : C_RED),
           "%lu/%lu received, %lu%% loss", (unsigned long)sPingOk, (unsigned long)sent, (unsigned long)loss);
  if (sPingOk)
    conPrint(C_TEXT, "min %lu  avg %lu  max %lu ms", (unsigned long)sPingMin,
             (unsigned long)(sPingSum / sPingOk), (unsigned long)sPingMax);
}

// ---------------------------------------------------------------- traceroute (ICMP)
static uint16_t inetCksum(const uint8_t *d, int len) {
  uint32_t sum = 0;
  for (int i = 0; i + 1 < len; i += 2) sum += (uint32_t)d[i] << 8 | d[i + 1];
  if (len & 1) sum += (uint32_t)d[len - 1] << 8;
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

static void doTrace(const uint8_t dst[4]) {
  conPrint(C_CYAN, "TRACE to %s", ipToStr(dst).c_str());
  int s = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (s < 0) { conPrint(C_RED, "socket error %d", errno); return; }
  struct timeval tv = {0, 300000};
  lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  struct sockaddr_in to = {};
  to.sin_family = AF_INET;
  memcpy(&to.sin_addr, dst, 4);
  const uint16_t id = (uint16_t)esp_random();
  bool reached = false;

  for (int ttl = 1; ttl <= 24 && !reached && !sAbort; ttl++) {
    lwip_setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);
    bool got = false;
    for (int attempt = 0; attempt < 2 && !got && !sAbort; attempt++) {
      uint8_t pkt[40];
      memset(pkt, 0xA5, sizeof pkt);
      const uint16_t seq = (uint16_t)(ttl * 4 + attempt);
      pkt[0] = 8; pkt[1] = 0; pkt[2] = 0; pkt[3] = 0;
      pkt[4] = id >> 8; pkt[5] = id & 0xff; pkt[6] = seq >> 8; pkt[7] = seq & 0xff;
      uint16_t ck = inetCksum(pkt, sizeof pkt);
      pkt[2] = ck >> 8; pkt[3] = ck & 0xff;
      uint32_t t0 = micros();
      if (lwip_sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) < 0) {
        conPrint(C_RED, "%2d  send failed (%d)", ttl, errno);
        break;
      }
      while (!got && !sAbort && micros() - t0 < 1500000UL) {
        uint8_t buf[128];
        struct sockaddr_in from = {};
        socklen_t fl = sizeof from;
        int n = lwip_recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 28) continue;                       // timeout slice or runt
        int ihl = (buf[0] & 0x0f) * 4;
        if (n < ihl + 8) continue;
        const uint8_t *ic = buf + ihl;
        bool mine = false, last = false;
        if (ic[0] == 0) {                           // echo reply
          mine = ic[4] == pkt[4] && ic[5] == pkt[5] && ic[6] == pkt[6] && ic[7] == pkt[7];
          last = true;
        } else if (ic[0] == 11 || ic[0] == 3) {     // time exceeded / unreachable
          const uint8_t *in = ic + 8;
          int iihl = (in[0] & 0x0f) * 4;
          if (n >= ihl + 8 + iihl + 8) {
            const uint8_t *q = in + iihl;
            mine = q[0] == 8 && q[4] == pkt[4] && q[5] == pkt[5] && q[6] == pkt[6] && q[7] == pkt[7];
          }
          last = ic[0] == 3;
        }
        if (!mine) continue;
        got = true;
        uint32_t us = micros() - t0;
        uint8_t hop[4];
        memcpy(hop, &from.sin_addr, 4);
        conPrint(last ? C_GREEN : C_TEXT, "%2d  %-15s  %lu.%lu ms%s", ttl, ipToStr(hop).c_str(),
                 (unsigned long)(us / 1000), (unsigned long)((us % 1000) / 100),
                 ic[0] == 3 ? "  unreachable" : "");
        if (last) reached = true;
      }
    }
    if (!got && !sAbort) conPrint(C_DIM, "%2d  *", ttl);
  }
  lwip_close(s);
  if (reached) conPrint(C_GREEN, "Done");
  else if (!sAbort) conPrint(C_YELLOW, "Destination not reached");
}

// ---------------------------------------------------------------- DNS + web
static int tcpConnect(const struct in_addr &ip, uint16_t port, int timeoutMs, uint32_t &ms) {
  int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int fl = lwip_fcntl(s, F_GETFL, 0);
  lwip_fcntl(s, F_SETFL, fl | O_NONBLOCK);
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr = ip;
  uint32_t t0 = millis();
  int r = lwip_connect(s, (struct sockaddr *)&a, sizeof a);
  if (r < 0 && errno == EINPROGRESS) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    r = lwip_select(s + 1, NULL, &w, NULL, &tv);
    if (r > 0) {
      int err = 0;
      socklen_t l = sizeof err;
      lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
      r = err ? -1 : 0;
    } else {
      r = -1;
    }
  }
  ms = millis() - t0;
  if (r < 0) { lwip_close(s); return -1; }
  lwip_fcntl(s, F_SETFL, fl);
  return s;
}

static bool resolve(const char *host, struct in_addr &out, uint32_t &ms) {
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  uint32_t t0 = millis();
  int r = lwip_getaddrinfo(host, NULL, &hints, &res);
  ms = millis() - t0;
  if (r != 0 || !res) return false;
  out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
  lwip_freeaddrinfo(res);
  return true;
}

static void doDnsWeb() {
  NetState st;
  if (!needIp(st)) return;
  conPrint(C_CYAN, "DNS server %s", ipToStr(st.dns1).c_str());
  static const char *const hosts[] = {"www.google.com", "www.microsoft.com"};
  bool dnsOk = false;
  for (auto h : hosts) {
    if (sAbort) return;
    struct in_addr ip;
    uint32_t ms;
    if (resolve(h, ip, ms)) {
      dnsOk = true;
      conPrint(C_GREEN, "%s", h);
      conPrint(C_TEXT, "  -> %s  (%lu ms)", ipToStr((const uint8_t *)&ip).c_str(), (unsigned long)ms);
    } else {
      conPrint(C_RED, "%s  lookup FAILED", h);
    }
  }
  if (!dnsOk) { conPrint(C_RED, "DNS is not working"); return; }

  // Captive-portal style check: a clean internet path answers HTTP 204
  const char *host = "connectivitycheck.gstatic.com";
  struct in_addr ip;
  uint32_t ms;
  conPrint(C_CYAN, "HTTP check");
  if (!resolve(host, ip, ms)) { conPrint(C_RED, "  lookup failed"); return; }
  int s = tcpConnect(ip, 80, 4000, ms);
  if (s < 0) { conPrint(C_RED, "  TCP 80 connect failed"); return; }
  conPrint(C_TEXT, "  TCP connect %lu ms", (unsigned long)ms);
  char req[128];
  int rl = snprintf(req, sizeof req, "GET /generate_204 HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
  struct timeval tv = {4, 0};
  lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  lwip_send(s, req, rl, 0);
  char resp[64] = "";
  int n = lwip_recv(s, resp, sizeof resp - 1, 0);
  lwip_close(s);
  if (n < 12) { conPrint(C_RED, "  no HTTP response"); return; }
  resp[n] = 0;
  int code = atoi(resp + 9);
  if (code == 204) conPrint(C_GREEN, "  HTTP 204 - INTERNET OK");
  else conPrint(C_YELLOW, "  HTTP %d - captive portal/proxy?", code);
}

// ---------------------------------------------------------------- DHCP scan
static void doDhcpScan() {
  powerHoldAwake(true);                           // wake PHY before looking for a link
  NetState st;
  netGetState(st);
  if (!st.link) {
    conPrint(C_YELLOW, "Waiting for link…");
    for (int i = 0; i < 60 && !sAbort; i++) {     // up to ~3 s after a cable-sleep wake
      if (!waitMs(50)) { powerHoldAwake(false); return; }
      netGetState(st);
      if (st.link) break;
    }
  }
  if (!st.link) {
    conPrint(C_RED, "No link - plug in a cable");
    powerHoldAwake(false);
    return;
  }
  conPrint(C_CYAN, "Sending DHCP DISCOVER...");
  netClearOffers();
  netRenewDhcp();
  for (int i = 0; i < 12; i++) {                  // collect offers for ~6 s
    if (!waitMs(500)) { powerHoldAwake(false); return; }
  }
  DhcpOffer offers[MAX_OFFERS];
  int n = netGetOffers(offers);
  if (n == 0) {
    conPrint(C_RED, "No DHCP server answered");
    return;
  }
  for (int i = 0; i < n; i++) {
    conPrint(C_GREEN, "Server %s", ipToStr(offers[i].server).c_str());
    conPrint(C_TEXT, "  via %s", macToStr(offers[i].mac).c_str());
    conPrint(C_TEXT, "  offers %s", ipToStr(offers[i].offered).c_str());
  }
  if (n > 1) conPrint(C_RED, "!! %d DHCP SERVERS - rogue server?", n);
  else conPrint(C_GREEN, "Single DHCP server - OK");
  DhcpInfo d;
  netGetDhcp(d);
  if (d.valid) {
    if (!ipIsZero(d.relay)) conPrint(C_TEXT, "Relay agent %s", ipToStr(d.relay).c_str());
    conPrint(C_TEXT, "Lease %luh %lum  %s", (unsigned long)(d.leaseSec / 3600),
             (unsigned long)((d.leaseSec % 3600) / 60), d.domain);
    if (d.tftp[0]) conPrint(C_TEXT, "TFTP %s", d.tftp);
    if (!ipIsZero(d.ntp)) conPrint(C_TEXT, "NTP %s", ipToStr(d.ntp).c_str());
  }
  powerHoldAwake(false);
}

// ---------------------------------------------------------------- blink the switch port LED
static void doBlink() {
  powerHoldAwake(true);                           // keep cable-sleep from fighting the flaps
  conPrint(C_CYAN, "Flapping link - watch the switch");
  conPrint(C_DIM, "Port LED goes off/on 6 times");
  netHoldInfo(true);                              // keep CDP/LLDP data across the flaps
  for (int i = 1; i <= 6 && !sAbort; i++) {
    netPhyPower(false);
    conPrint(C_ORANGE, "%d/6  link DOWN", i);
    waitMs(1200);
    netPhyPower(true);
    conPrint(C_GREEN, "%d/6  link UP", i);
    waitMs(2800);
  }
  netPhyPower(true);
  NetState st;
  for (int i = 0; i < 100; i++) {                 // wait for the link to settle (max 5 s)
    netGetState(st);
    if (st.link) break;
    delay(50);
  }
  netHoldInfo(false);
  powerHoldAwake(false);
  conPrint(C_TEXT, "Done - link %s", st.link ? "restored" : "still down");
}

// ---------------------------------------------------------------- launcher
static void toolTask(void *arg) {
  ToolId id = (ToolId)(intptr_t)arg;
  NetState st;
  switch (id) {
    case TOOL_PING_GW:
      if (needIp(st)) doPing(st.gw, 8);
      break;
    case TOOL_PING_INET:
      if (needIp(st)) doPing(INET_TARGET, 8);
      break;
    case TOOL_TRACE:
      if (needIp(st)) doTrace(INET_TARGET);
      break;
    case TOOL_DNS_WEB:   doDnsWeb(); break;
    case TOOL_DHCP_SCAN: doDhcpScan(); break;
    case TOOL_BLINK_PORT: doBlink(); break;
    default: break;
  }
  if (sAbort) conPrint(C_YELLOW, "Stopped");
  powerHoldAwake(false);                          // safety net if a tool returned early
  powerNoteEvent();                               // wake the screen for the finished result
  sBusy = false;
  vTaskDelete(NULL);
}

void toolStart(ToolId id) {
  if (sBusy || id < TOOL_PING_GW || id >= TOOL_QR) return;
  static const char *const titles[TOOL_COUNT] = {
    "PING GATEWAY", "PING 8.8.8.8", "TRACEROUTE", "DNS + WEB", "DHCP SCAN", "BLINK PORT", "QR", "EXTRA TOOLS",
  };
  conReset(titles[id]);
  sAbort = false;
  sBusy = true;
  // Any diagnostic that needs the wire must leave cable-sleep first.
  powerHoldAwake(true);
  if (xTaskCreate(toolTask, "tool", 8192, (void *)(intptr_t)id, 2, NULL) != pdPASS) {
    powerHoldAwake(false);
    sBusy = false;
  }
}

void toolsBegin() {
  sConLock = xSemaphoreCreateMutex();
  sPingDone = xSemaphoreCreateBinary();
}
