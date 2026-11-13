// Ethernet (W5500, raw MAC mode) + passive sniffer for CDP / LLDP / DHCP / 802.1Q
#pragma once
#include <Arduino.h>
#include "esp_netif.h"

struct NetState {
  bool     started;        // driver came up (module detected)
  bool     link;
  int      speedMbps;      // 10 / 100
  bool     fullDuplex;
  bool     hasIp;
  uint8_t  ip[4], mask[4], gw[4], dns1[4], dns2[4];
  uint8_t  mac[6];
  uint32_t linkUpMs;       // millis() when link came up
  uint32_t rxFrames;       // frames seen since link up
  uint16_t vlanTags[8];    // 802.1Q tags seen on the wire (trunk port hint)
  uint8_t  vlanTagCount;
};

// One discovered neighbour (the switch port we are plugged into)
struct Neighbor {
  bool     valid;
  char     device[48];     // switch hostname
  char     port[40];       // port id, e.g. GigabitEthernet1/0/12
  char     portDesc[40];   // LLDP port description
  char     platform[48];   // model
  char     software[64];   // first line of the version string
  char     mgmtIp[16];
  char     caps[20];
  char     vtpDomain[24];
  int      nativeVlan;     // -1 = unknown
  int      voiceVlan;      // -1 = unknown
  int      duplex;         // -1 unknown, 0 half, 1 full (as reported by switch)
  int      poeMw;          // -1 unknown
  uint8_t  srcMac[6];
  uint32_t lastSeenMs;
  uint32_t count;
};

struct DhcpInfo {
  bool     valid;
  uint8_t  server[4];      // option 54
  uint8_t  serverMac[6];   // L2 source of the ACK (server or relay)
  uint8_t  relay[4];       // giaddr
  uint8_t  dns[2][4];
  uint32_t leaseSec;
  char     domain[40];
  char     tftp[32];       // option 66 / 150
  uint8_t  ntp[4];
};

struct DhcpOffer {
  uint8_t  server[4];
  uint8_t  mac[6];
  uint8_t  offered[4];
};
#define MAX_OFFERS 4

bool        netBegin();                 // false if the W5500 was not found
void        netGetState(NetState &s);
void        netGetCdp(Neighbor &n);
void        netGetLldp(Neighbor &n);
void        netGetDhcp(DhcpInfo &d);
int         netGetOffers(DhcpOffer *out);   // returns count (<= MAX_OFFERS)
void        netClearOffers();
void        netRenewDhcp();             // restart the DHCP client (sends a fresh DISCOVER)
void        netPhyPower(bool on);       // drop / restore the link (blinks the switch port LED)
void        netHoldInfo(bool hold);     // keep discovered info across link flaps
esp_netif_t *netIf();
void        netBusLock();               // wrap every display SPI access in Lock/Unlock
void        netBusUnlock();

String ipToStr(const uint8_t ip[4]);
String macToStr(const uint8_t mac[6]);
bool   ipIsZero(const uint8_t ip[4]);
int    maskToPrefix(const uint8_t mask[4]);

// Best view of the connected switch port: CDP preferred, gaps filled from LLDP.
// proto is set to "CDP", "LLDP" or "CDP+LLDP". Returns false if nothing heard yet.
bool   netBestNeighbor(Neighbor &out, const char *&proto);
String shortPort(const char *port);     // GigabitEthernet1/0/12 -> Gi1/0/12
