// W5500 module detection and Ethernet initialization using the ESP-IDF driver.
// Raw-MAC receive processing extracts CDP, LLDP, DHCP, and 802.1Q information
// before forwarding traffic to lwIP; display access shares the SPI bus.
#include "net.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_system.h"
#if ESP_IDF_VERSION_MAJOR >= 5
#include "esp_mac.h"
#endif
#include "driver/spi_master.h"
#include "driver/gpio.h"

#ifndef ETH_SPI_HOST
#define ETH_SPI_HOST SPI3_HOST   // VSPI: the bus the Core2 shares between LCD, SD and M-Bus
#endif

static esp_eth_handle_t  sEth   = nullptr;
static esp_eth_mac_t    *sMac   = nullptr;
static esp_netif_t      *sNetif = nullptr;
static SemaphoreHandle_t sLock  = nullptr;

static NetState  sState;
static Neighbor  sCdp, sLldp;
static DhcpInfo  sDhcp;
static DhcpOffer sOffers[MAX_OFFERS];
static int       sOfferCount = 0;
static volatile bool sHold = false;
static spi_device_handle_t sBusGuard = nullptr;

struct LanProfile {
  const char *name;
  int cs, reset, interrupt;
};

#ifdef ETH_AUTO_DETECT
#ifndef NO_SOUND
#error "Ethernet auto-detection requires NO_SOUND: GPIO0 is a reset candidate"
#endif
static const LanProfile sProfiles[] = {
  {"LAN Module 13.2", 33, 0, 35},
  {"LAN / PoE Base", 26, 19, 34},
};
#else
static const LanProfile sProfiles[] = {
  {"Fixed LAN profile", ETH_PIN_CS, ETH_PIN_RST, ETH_PIN_INT},
};
#endif
static const LanProfile *sProfile = nullptr;

#define LOCK()   xSemaphoreTake(sLock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(sLock)

// ---------------------------------------------------------------- helpers
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// Copy the first line of a (not NUL terminated) protocol string, printable chars only
static void copyStr(char *dst, size_t cap, const uint8_t *src, int len) {
  size_t n = 0;
  for (int i = 0; i < len && n + 1 < cap; i++) {
    char c = (char)src[i];
    if (c == '\n' || c == '\r' || c == 0) break;
    if (c < 32 || c > 126) c = '?';
    dst[n++] = c;
  }
  dst[n] = 0;
}

static void fmtIp(char *dst, size_t cap, const uint8_t *ip) {
  snprintf(dst, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}
static void fmtMac(char *dst, size_t cap, const uint8_t *m) {
  snprintf(dst, cap, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

String ipToStr(const uint8_t ip[4]) { char b[16]; fmtIp(b, sizeof b, ip); return String(b); }
String macToStr(const uint8_t mac[6]) { char b[18]; fmtMac(b, sizeof b, mac); return String(b); }
bool ipIsZero(const uint8_t ip[4]) { return !(ip[0] | ip[1] | ip[2] | ip[3]); }
int maskToPrefix(const uint8_t mask[4]) {
  int n = 0;
  for (int i = 0; i < 4; i++)
    for (int b = 7; b >= 0; b--)
      if (mask[i] & (1 << b)) n++; else return n;
  return n;
}

static void resetNeighbor(Neighbor &n) {
  memset(&n, 0, sizeof n);
  n.nativeVlan = n.voiceVlan = n.duplex = n.poeMw = -1;
}

// ---------------------------------------------------------------- CDP
static void parseCdp(const uint8_t *p, int len, const uint8_t *src) {
  if (len < 4) return;
  Neighbor n;
  resetNeighbor(n);
  p += 4; len -= 4;                       // version, ttl, checksum
  while (len >= 4) {
    uint16_t type = be16(p), tl = be16(p + 2);
    if (tl < 4 || tl > len) break;
    const uint8_t *v = p + 4;
    int vl = tl - 4;
    switch (type) {
      case 0x0001: copyStr(n.device, sizeof n.device, v, vl); break;
      case 0x0003: copyStr(n.port, sizeof n.port, v, vl); break;
      case 0x0005: copyStr(n.software, sizeof n.software, v, vl); break;
      case 0x0006: copyStr(n.platform, sizeof n.platform, v, vl); break;
      case 0x0009: copyStr(n.vtpDomain, sizeof n.vtpDomain, v, vl); break;
      case 0x000a: if (vl >= 2) n.nativeVlan = be16(v); break;
      case 0x000b: if (vl >= 1) n.duplex = v[0] ? 1 : 0; break;
      case 0x000e: if (vl >= 3) n.voiceVlan = be16(v + 1); break;   // VoIP VLAN reply
      case 0x001a: if (vl >= 8) n.poeMw = (int)be32(v + 4); break;  // power available
      case 0x0004:
        if (vl >= 4) {
          uint32_t c = be32(v);
          snprintf(n.caps, sizeof n.caps, "%s%s%s%s%s",
                   (c & 0x01) ? "Router " : "", (c & 0x08) ? "Switch " : "",
                   (c & 0x02) ? "Bridge " : "", (c & 0x10) ? "Host " : "",
                   (c & 0x80) ? "Phone" : "");
        }
        break;
      case 0x0002:      // addresses
      case 0x0016: {    // management addresses
        if (vl < 4) break;
        uint32_t cnt = be32(v);
        const uint8_t *a = v + 4;
        int al = vl - 4;
        while (cnt-- && al >= 2) {
          uint8_t ptype = a[0], plen = a[1];
          if (al < 2 + plen + 2) break;
          const uint8_t *proto = a + 2;
          uint16_t alen = be16(a + 2 + plen);
          if (al < 2 + plen + 2 + alen) break;
          const uint8_t *addr = a + 2 + plen + 2;
          if (ptype == 1 && plen == 1 && proto[0] == 0xCC && alen == 4 &&
              (type == 0x0016 || !n.mgmtIp[0])) {
            fmtIp(n.mgmtIp, sizeof n.mgmtIp, addr);
          }
          a += 2 + plen + 2 + alen;
          al -= 2 + plen + 2 + alen;
        }
        break;
      }
    }
    p += tl; len -= tl;
  }
  memcpy(n.srcMac, src, 6);
  n.valid = true;
  n.lastSeenMs = millis();
  LOCK();
  n.count = sCdp.count + 1;
  sCdp = n;
  UNLOCK();
}

// ---------------------------------------------------------------- LLDP
static void parseLldp(const uint8_t *p, int len, const uint8_t *src) {
  Neighbor n;
  resetNeighbor(n);
  char chassis[48] = "";
  bool portIsMac = false;
  while (len >= 2) {
    uint16_t hdr = be16(p);
    int type = hdr >> 9, tl = hdr & 0x1ff;
    p += 2; len -= 2;
    if (tl > len || type == 0) break;
    const uint8_t *v = p;
    switch (type) {
      case 1:   // chassis id
        if (tl >= 2) {
          if (v[0] == 4 && tl == 7) fmtMac(chassis, sizeof chassis, v + 1);
          else if (v[0] == 5 && tl == 6 && v[1] == 1) fmtIp(chassis, sizeof chassis, v + 2);
          else copyStr(chassis, sizeof chassis, v + 1, tl - 1);
        }
        break;
      case 2:   // port id
        if (tl >= 2) {
          if (v[0] == 3 && tl == 7) { fmtMac(n.port, sizeof n.port, v + 1); portIsMac = true; }
          else copyStr(n.port, sizeof n.port, v + 1, tl - 1);
        }
        break;
      case 4: copyStr(n.portDesc, sizeof n.portDesc, v, tl); break;
      case 5: copyStr(n.device, sizeof n.device, v, tl); break;
      case 6: copyStr(n.platform, sizeof n.platform, v, tl); break;
      case 7:
        if (tl >= 4) {
          uint16_t c = be16(v + 2);     // enabled capabilities
          snprintf(n.caps, sizeof n.caps, "%s%s%s%s%s",
                   (c & 0x10) ? "Router " : "", (c & 0x04) ? "Switch " : "",
                   (c & 0x08) ? "WLAN-AP " : "", (c & 0x20) ? "Phone " : "",
                   (c & 0x80) ? "Host" : "");
        }
        break;
      case 8:   // management address
        if (tl >= 6 && v[0] == 5 && v[1] == 1 && !n.mgmtIp[0]) fmtIp(n.mgmtIp, sizeof n.mgmtIp, v + 2);
        break;
      case 127: // organisationally specific
        if (tl >= 4) {
          uint32_t oui = (uint32_t)v[0] << 16 | (uint32_t)v[1] << 8 | v[2];
          uint8_t sub = v[3];
          if (oui == 0x0080C2 && sub == 1 && tl >= 6) {
            n.nativeVlan = be16(v + 4);                         // port VLAN id
          } else if (oui == 0x0012BB && sub == 2 && tl >= 8 && v[4] == 1) {
            uint32_t pol = (uint32_t)v[5] << 16 | (uint32_t)v[6] << 8 | v[7];
            n.voiceVlan = (pol >> 9) & 0x0fff;                  // LLDP-MED voice policy
          } else if (oui == 0x00120F && sub == 1 && tl >= 9) {
            uint16_t mau = be16(v + 7);                         // operational MAU type
            if (mau == 10 || mau == 15 || mau == 29) n.duplex = 0;
            else if (mau == 11 || mau == 16 || mau == 30) n.duplex = 1;
          }
        }
        break;
    }
    p += tl; len -= tl;
  }
  if (!n.device[0]) strlcpy(n.device, chassis, sizeof n.device);
  // Many switches send the port MAC as the port id and the name as the description
  if (portIsMac && n.portDesc[0]) {
    char tmp[40];
    strlcpy(tmp, n.port, sizeof tmp);
    strlcpy(n.port, n.portDesc, sizeof n.port);
    strlcpy(n.portDesc, tmp, sizeof n.portDesc);
  }
  if (n.nativeVlan == 0) n.nativeVlan = -1;
  if (n.voiceVlan == 0) n.voiceVlan = -1;
  memcpy(n.srcMac, src, 6);
  n.valid = true;
  n.lastSeenMs = millis();
  LOCK();
  n.count = sLldp.count + 1;
  sLldp = n;
  UNLOCK();
}

// ---------------------------------------------------------------- DHCP (server -> client)
static void parseDhcp(const uint8_t *b, int len, const uint8_t *src) {
  if (len < 244 || b[0] != 2) return;
  if (memcmp(b + 28, sState.mac, 6) != 0) return;            // not a reply to us
  if (be32(b + 236) != 0x63825363) return;
  DhcpInfo d;
  memset(&d, 0, sizeof d);
  int msgType = 0;
  memcpy(d.server, b + 20, 4);                                // siaddr until option 54 says otherwise
  memcpy(d.relay, b + 24, 4);
  int i = 240;
  while (i < len) {
    uint8_t o = b[i++];
    if (o == 255) break;
    if (o == 0) continue;
    if (i >= len) break;
    int l = b[i++];
    if (i + l > len) break;
    const uint8_t *v = b + i;
    switch (o) {
      case 53: if (l >= 1) msgType = v[0]; break;
      case 54: if (l >= 4) memcpy(d.server, v, 4); break;
      case 51: if (l >= 4) d.leaseSec = be32(v); break;
      case 6:
        if (l >= 4) memcpy(d.dns[0], v, 4);
        if (l >= 8) memcpy(d.dns[1], v + 4, 4);
        break;
      case 15: copyStr(d.domain, sizeof d.domain, v, l); break;
      case 42: if (l >= 4) memcpy(d.ntp, v, 4); break;
      case 66: copyStr(d.tftp, sizeof d.tftp, v, l); break;
      case 150: if (l >= 4 && !d.tftp[0]) fmtIp(d.tftp, sizeof d.tftp, v); break;
    }
    i += l;
  }
  memcpy(d.serverMac, src, 6);
  LOCK();
  if (msgType == 2) {                                         // OFFER
    bool known = false;
    for (int k = 0; k < sOfferCount; k++)
      if (!memcmp(sOffers[k].server, d.server, 4)) known = true;
    if (!known && sOfferCount < MAX_OFFERS) {
      memcpy(sOffers[sOfferCount].server, d.server, 4);
      memcpy(sOffers[sOfferCount].mac, src, 6);
      memcpy(sOffers[sOfferCount].offered, b + 16, 4);
      sOfferCount++;
    }
  } else if (msgType == 5) {                                  // ACK
    d.valid = true;
    sDhcp = d;
  }
  UNLOCK();
}

static void parseIpv4(const uint8_t *p, int len, const uint8_t *src) {
  if (len < 28 || (p[0] >> 4) != 4 || p[9] != 17) return;
  int ihl = (p[0] & 0x0f) * 4;
  if (ihl < 20 || len < ihl + 8) return;
  const uint8_t *udp = p + ihl;
  if (be16(udp) != 67 || be16(udp + 2) != 68) return;
  parseDhcp(udp + 8, len - ihl - 8, src);
}

static void noteVlan(uint16_t vid) {
  if (vid == 0 || vid == 0x0fff) return;
  for (int i = 0; i < sState.vlanTagCount; i++)
    if (sState.vlanTags[i] == vid) return;
  LOCK();
  if (sState.vlanTagCount < 8) sState.vlanTags[sState.vlanTagCount++] = vid;
  UNLOCK();
}

static void sniff(const uint8_t *f, uint32_t len) {
  uint32_t off = 12;
  uint16_t et = be16(f + off);
  if (et == 0x8100 && len >= 18) {
    noteVlan(be16(f + 14) & 0x0fff);
    off += 4;
    et = be16(f + off);
  }
  off += 2;
  if (off >= len) return;
  const uint8_t *p = f + off;
  int plen = (int)(len - off);
  if (et <= 1500) {                       // 802.3 + LLC/SNAP
    if (et < plen) plen = et;
    static const uint8_t cdpSnap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x0c, 0x20, 0x00};
    if (plen > 8 && !memcmp(p, cdpSnap, 8)) parseCdp(p + 8, plen - 8, f + 6);
  } else if (et == 0x88cc) {
    parseLldp(p, plen, f + 6);
  } else if (et == 0x0800) {
    parseIpv4(p, plen, f + 6);
  }
}

// Every received frame comes through here (driver RX task) before lwIP sees it
static esp_err_t ethInput(esp_eth_handle_t h, uint8_t *buf, uint32_t len, void *priv) {
  if (len >= 14) {
    sState.rxFrames++;
    bool forUs = (buf[0] & 1) || !memcmp(buf, sState.mac, 6);
    sniff(buf, len);
    if (!forUs) {                         // promiscuous leftovers: not for the IP stack
      free(buf);
      return ESP_OK;
    }
  }
  return esp_netif_receive((esp_netif_t *)priv, buf, len, NULL);
}

// ---------------------------------------------------------------- events
static void onEthEvent(void *, esp_event_base_t, int32_t id, void *) {
  if (id == ETHERNET_EVENT_CONNECTED) {
    eth_speed_t sp = ETH_SPEED_10M;
    eth_duplex_t dp = ETH_DUPLEX_HALF;
    esp_eth_ioctl(sEth, ETH_CMD_G_SPEED, &sp);
    esp_eth_ioctl(sEth, ETH_CMD_G_DUPLEX_MODE, &dp);
    LOCK();
    sState.link = true;
    sState.speedMbps = (sp == ETH_SPEED_100M) ? 100 : 10;
    sState.fullDuplex = (dp == ETH_DUPLEX_FULL);
    if (!sHold) {                         // new port: forget what we knew
      sState.linkUpMs = millis();
      sState.rxFrames = 0;
      sState.vlanTagCount = 0;
      uint32_t c = sCdp.count, l = sLldp.count;
      resetNeighbor(sCdp);
      resetNeighbor(sLldp);
      sCdp.count = c; sLldp.count = l;
      memset(&sDhcp, 0, sizeof sDhcp);
      sOfferCount = 0;
    }
    UNLOCK();
    Serial.printf("LAN link up: %d Mbps %s duplex\n", sp == ETH_SPEED_100M ? 100 : 10,
                  dp == ETH_DUPLEX_FULL ? "full" : "half");
  } else if (id == ETHERNET_EVENT_DISCONNECTED) {
    LOCK();
    sState.link = false;
    sState.hasIp = false;
    UNLOCK();
    Serial.println("LAN link down");
  }
}

static void onIpEvent(void *, esp_event_base_t, int32_t id, void *data) {
  if (id == IP_EVENT_ETH_GOT_IP) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    esp_netif_dns_info_t d1 = {}, d2 = {};
    esp_netif_get_dns_info(sNetif, ESP_NETIF_DNS_MAIN, &d1);
    esp_netif_get_dns_info(sNetif, ESP_NETIF_DNS_BACKUP, &d2);
    LOCK();
    memcpy(sState.ip, &e->ip_info.ip.addr, 4);
    memcpy(sState.mask, &e->ip_info.netmask.addr, 4);
    memcpy(sState.gw, &e->ip_info.gw.addr, 4);
    memcpy(sState.dns1, &d1.ip.u_addr.ip4.addr, 4);
    memcpy(sState.dns2, &d2.ip.u_addr.ip4.addr, 4);
    sState.hasIp = true;
    UNLOCK();
    Serial.printf("LAN DHCP: IP=%s mask=%s gateway=%s\n", ipToStr(sState.ip).c_str(),
                  ipToStr(sState.mask).c_str(), ipToStr(sState.gw).c_str());
  } else if (id == IP_EVENT_ETH_LOST_IP) {
    LOCK();
    sState.hasIp = false;
    UNLOCK();
  }
}

// ---------------------------------------------------------------- bring-up
// Detect the chip itself, not Ethernet link or DHCP: this also works with no
// cable connected. VERSIONR is read-only and must repeatedly return 0x04.
// Probe devices are removed before the real MAC driver takes over the bus.
static bool probeLan(const LanProfile &profile) {
  digitalWrite(profile.reset, HIGH);
  pinMode(profile.reset, OUTPUT);
  digitalWrite(profile.reset, LOW);
  delay(2);                              // W5500 requires at least 500 us
  digitalWrite(profile.reset, HIGH);
  delay(100);                            // allow oscillator / reset recovery

  spi_device_interface_config_t dev = {};
  dev.command_bits = 16;                  // W5500 register address
  dev.address_bits = 8;                   // control byte: block 0, read, VDM
  dev.clock_speed_hz = 1000 * 1000;       // conservative speed for detection
  dev.spics_io_num = profile.cs;
  dev.queue_size = 1;
  spi_device_handle_t spi = nullptr;
  esp_err_t err = spi_bus_add_device(ETH_SPI_HOST, &dev, &spi);
  if (err != ESP_OK) {
    Serial.printf("LAN probe %s failed: %s\n", profile.name, esp_err_to_name(err));
    return false;
  }

  bool found = true;
  for (int i = 0; i < 3; ++i) {
    spi_transaction_t tx = {};
    tx.cmd = 0x0039;                      // common-register VERSIONR
    tx.addr = 0;
    tx.length = 8;
    tx.flags = SPI_TRANS_USE_RXDATA;
    err = spi_device_transmit(spi, &tx);
    if (err != ESP_OK || tx.rx_data[0] != 0x04) {
      found = false;
      break;
    }
  }
  spi_bus_remove_device(spi);
  digitalWrite(profile.cs, HIGH);
  Serial.printf("LAN probe %s: %s\n", profile.name, found ? "W5500 found" : "not found");
  return found;
}

bool netBegin() {
  sLock = xSemaphoreCreateMutex();
  memset(&sState, 0, sizeof sState);
  resetNeighbor(sCdp);
  resetNeighbor(sLldp);
  memset(&sDhcp, 0, sizeof sDhcp);

  esp_netif_init();
  esp_event_loop_create_default();        // "already created" is fine
  gpio_install_isr_service(0);            // "already installed" is fine

  // Keep every supported Ethernet CS inactive before touching the shared bus.
  for (const auto &profile : sProfiles) {
    digitalWrite(profile.cs, HIGH);
    pinMode(profile.cs, OUTPUT);
  }
  // An unused SD card must not answer transfers on the shared SPI bus.
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);

  // The display driver normally owns this bus already; INVALID_STATE is expected then.
  spi_bus_config_t bus = {};
  bus.mosi_io_num = ETH_PIN_MOSI;
  bus.miso_io_num = ETH_PIN_MISO;
  bus.sclk_io_num = ETH_PIN_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  esp_err_t busErr = spi_bus_initialize(ETH_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
  if (busErr != ESP_OK && busErr != ESP_ERR_INVALID_STATE) {
    Serial.printf("LAN SPI bus failed: %s\n", esp_err_to_name(busErr));
    return false;
  }
  if (busErr == ESP_ERR_INVALID_STATE) Serial.println("LAN sharing display SPI bus (expected)");

  for (const auto &profile : sProfiles) {
    if (probeLan(profile)) {
      sProfile = &profile;
      break;
    }
  }
  if (!sProfile) {
    Serial.println("LAN: no W5500 found on supported pin mappings");
    return false;
  }
  Serial.printf("LAN selected %s: SCK=%d MISO=%d MOSI=%d CS=%d RST=%d INT=%d\n",
                sProfile->name, ETH_PIN_SCK, ETH_PIN_MISO, ETH_PIN_MOSI,
                sProfile->cs, sProfile->reset, sProfile->interrupt);

  spi_device_interface_config_t dev = {};
  dev.mode = 0;
  dev.clock_speed_hz = 20 * 1000 * 1000;
  dev.spics_io_num = sProfile->cs;
  dev.queue_size = 20;
#if ESP_IDF_VERSION_MAJOR >= 5
  eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &dev);
#else
  dev.command_bits = 16;
  dev.address_bits = 8;
  spi_device_handle_t spi = nullptr;
  if (spi_bus_add_device(ETH_SPI_HOST, &dev, &spi) != ESP_OK) return false;
  eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(spi);
#endif
  w5500.int_gpio_num = sProfile->interrupt;

  // Guard device used by netBusLock(): lets the display (which pokes the SPI
  // registers directly) take the bus away from the Ethernet driver while drawing.
  spi_device_interface_config_t guard = {};
  guard.clock_speed_hz = 10 * 1000 * 1000;
  guard.spics_io_num = -1;
  guard.flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX;
  guard.queue_size = 1;
  if (spi_bus_add_device(ETH_SPI_HOST, &guard, &sBusGuard) != ESP_OK) {
    sBusGuard = nullptr;
    Serial.println("WARN: no SPI guard device - display and Ethernet may collide");
  }

  eth_mac_config_t macCfg = ETH_MAC_DEFAULT_CONFIG();
  macCfg.rx_task_stack_size = 4096;
  eth_phy_config_t phyCfg = ETH_PHY_DEFAULT_CONFIG();
  phyCfg.reset_gpio_num = sProfile->reset;

  sMac = esp_eth_mac_new_w5500(&w5500, &macCfg);
  esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phyCfg);
  if (!sMac || !phy) return false;

  esp_eth_config_t ethCfg = ETH_DEFAULT_CONFIG(sMac, phy);
  if (esp_eth_driver_install(&ethCfg, &sEth) != ESP_OK) {   // fails when no W5500 answers
    sEth = nullptr;
    sMac->del(sMac);                       // also stops the driver's RX task
    phy->del(phy);
    sMac = nullptr;
    return false;
  }

  // The W5500 has no burned-in MAC; use the ESP32's factory Ethernet MAC
  esp_read_mac(sState.mac, ESP_MAC_ETH);
  esp_eth_ioctl(sEth, ETH_CMD_S_MAC_ADDR, sState.mac);

  esp_netif_config_t nifCfg = ESP_NETIF_DEFAULT_ETH();
  sNetif = esp_netif_new(&nifCfg);
  esp_netif_attach(sNetif, esp_eth_new_netif_glue(sEth));
  esp_netif_set_hostname(sNetif, "m5-nettool");

  // Put our tap in front of the IP stack
  esp_eth_update_input_path(sEth, ethInput, sNetif);

  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, onEthEvent, NULL);
  esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, onIpEvent, NULL);

  // CDP/LLDP are sent to multicast MACs we are not a member of -> promiscuous
  bool on = true;
  esp_eth_ioctl(sEth, ETH_CMD_S_PROMISCUOUS, &on);
  if (esp_eth_start(sEth) != ESP_OK) return false;
  esp_eth_ioctl(sEth, ETH_CMD_S_PROMISCUOUS, &on);

  sState.started = true;
  Serial.printf("LAN W5500 initialized, MAC=%s; waiting for cable / DHCP\n", macToStr(sState.mac).c_str());
  return true;
}

// ---------------------------------------------------------------- accessors
const char *netModuleName() { return sState.started && sProfile ? sProfile->name : "No LAN module"; }
void netGetState(NetState &s) { LOCK(); s = sState; UNLOCK(); }
void netGetCdp(Neighbor &n)   { LOCK(); n = sCdp;   UNLOCK(); }
void netGetLldp(Neighbor &n)  { LOCK(); n = sLldp;  UNLOCK(); }
void netGetDhcp(DhcpInfo &d)  { LOCK(); d = sDhcp;  UNLOCK(); }
int netGetOffers(DhcpOffer *out) {
  LOCK();
  int n = sOfferCount;
  memcpy(out, sOffers, sizeof(DhcpOffer) * n);
  UNLOCK();
  return n;
}
void netClearOffers() { LOCK(); sOfferCount = 0; UNLOCK(); }
void netHoldInfo(bool hold) { sHold = hold; }
esp_netif_t *netIf() { return sNetif; }

// The display driver writes the SPI peripheral registers itself. Holding the bus
// through the IDF driver while it does so keeps the W5500 task out, and makes the
// IDF driver re-apply the W5500 settings on its next transfer.
void netBusLock()   { if (sBusGuard) spi_device_acquire_bus(sBusGuard, portMAX_DELAY); }
void netBusUnlock() { if (sBusGuard) spi_device_release_bus(sBusGuard); }

void netRenewDhcp() {
  if (!sNetif) return;
  esp_netif_dhcpc_stop(sNetif);
  esp_netif_dhcpc_start(sNetif);
}

// W5500 PHYCFGR: bit7 = !reset, bit6 = software-configured mode, bits5..3 = mode
// (110 = power down, 111 = auto-negotiate everything). A mode change is latched by
// pulsing the reset bit low.
void netPhyPower(bool on) {
  if (!sMac) return;
  const uint32_t reg = 0x002E0000;        // driver's address encoding for PHYCFGR
  uint32_t mode = 0x40 | ((on ? 0x7 : 0x6) << 3);
  sMac->write_phy_reg(sMac, 0, reg, mode | 0x80);
  sMac->write_phy_reg(sMac, 0, reg, mode);
  delay(2);
  sMac->write_phy_reg(sMac, 0, reg, mode | 0x80);
}

bool netBestNeighbor(Neighbor &out, const char *&proto) {
  Neighbor c, l;
  netGetCdp(c);
  netGetLldp(l);
  if (!c.valid && !l.valid) return false;
  if (c.valid && l.valid) {
    out = c;
    proto = "CDP+LLDP";
    if (!out.portDesc[0]) strlcpy(out.portDesc, l.portDesc, sizeof out.portDesc);
    if (!out.mgmtIp[0]) strlcpy(out.mgmtIp, l.mgmtIp, sizeof out.mgmtIp);
    if (out.nativeVlan < 0) out.nativeVlan = l.nativeVlan;
    if (out.voiceVlan < 0) out.voiceVlan = l.voiceVlan;
    if (out.duplex < 0) out.duplex = l.duplex;
    if (l.lastSeenMs > out.lastSeenMs) out.lastSeenMs = l.lastSeenMs;
  } else if (c.valid) {
    out = c; proto = "CDP";
  } else {
    out = l; proto = "LLDP";
  }
  return true;
}

String shortPort(const char *port) {
  static const char *const map[][2] = {
    {"TwentyFiveGigE", "Twe"}, {"TenGigabitEthernet", "Te"}, {"FortyGigabitEthernet", "Fo"},
    {"HundredGigE", "Hu"}, {"GigabitEthernet", "Gi"}, {"FastEthernet", "Fa"},
    {"Ethernet", "Eth"}, {"Port-channel", "Po"},
  };
  String s(port);
  for (auto &m : map) {
    if (s.startsWith(m[0])) return String(m[1]) + s.substring(strlen(m[0]));
  }
  return s;
}
