#pragma once

// Copy this file to secrets.h and fill in your local values.
// secrets.h is ignored by Git.

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const char* HTTP_KEY = "CHANGE_ME_LONG_RANDOM_KEY";

// target PC MAC address (magic packets are validated and built from this)
uint8_t targetMac[6] = {0x00,0x11,0x22,0x33,0x44,0x55};

const char* DUCKDNS_DOMAIN = "your-duckdns-subdomain";
const char* DUCKDNS_TOKEN = "your-duckdns-token";

// healthchecks.io ping URL (free account -> new check -> copy ping URL, keep http://)
const char* HC_PING_URL = "http://hc-ping.com/your-check-uuid";
