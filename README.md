# ESP32 WoL Relay

Wake a sleeping PC over the internet using an ESP32 as an always-on Wake-on-LAN relay inside your local network.

## Why

Consumer routers generally can't forward WAN traffic to the LAN broadcast address, and forwarding to the PC's IP stops working minutes after it falls asleep (its ARP entry expires). This relay solves both: the ESP32 stays awake 24/7 on a static LAN IP, receives the magic packet from the internet, validates it, and re-broadcasts it locally.

## How it works

```
Phone (4G / anywhere)
    │  UDP magic packet
    ▼
your-host.duckdns.org:9  ──►  modem WAN :9
    │  port forward (UDP 9 → ESP32 static IP :9)
    ▼
ESP32 relay ── validates target MAC ──►  LAN broadcast x.x.x.255:9  ──►  PC wakes
```

- **UDP relay** — packets arriving on port 9 are only re-broadcast if they are a valid magic packet for the configured target MAC.
- **HTTP API** — secondary wake path plus health/diagnostics endpoints.
- **DuckDNS updater** — keeps your hostname pointed at the (dynamic) WAN IP, refreshed every 5 minutes.

## Reliability

Built to run unattended for months. Every failure mode found in the field got a countermeasure:

| Mechanism | Covers |
|---|---|
| Loop watchdog task (second core, 30 s) | Stuck `loop()` / stale sockets |
| Reconnect + clean restart on WiFi drop | Router reboots |
| 45 s boot timeout → restart | Power outage where modem boots slower than ESP |
| Daily preventive restart, low-heap restart | Slow degradation, heap fragmentation |
| Dead man's switch ([healthchecks.io](https://healthchecks.io)) | Pings every 5 min with `rssi/heap/uptime` telemetry; you get an alert with the exact time the device went silent |
| Zombie-connection detector | 3 consecutive failed pings while WiFi still claims "connected" → clean restart (with backoff during ISP outages) |
| Persistent NVS event ring (`/log`) | Post-mortem for the period the device had no connectivity — survives power loss |

## Hardware

- Any ESP32 dev board (Arduino IDE board: **ESP32 Dev Module**)
- A solid 5V / 1A+ USB power adapter. **Do not power it from a PC's USB port** — the port may power down when the PC sleeps, which is a self-defeating failure mode for a WoL relay.

## Setup

1. Copy `secrets.example.h` → `secrets.h` and fill in:
   - WiFi SSID/password (2.4 GHz — ESP32 has no 5 GHz)
   - `HTTP_KEY` (long random string, protects `/wake` and `/reboot`)
   - DuckDNS domain + token
   - healthchecks.io ping URL (create a free check: period **5 min**, grace **10 min**)
   - Target PC MAC address
2. Adjust the static IP block at the top of the sketch for your subnet (pick an IP outside the DHCP pool).
3. Flash with Arduino IDE.
4. Modem/router: forward **UDP external 9 → ESP32 static IP:9**.
5. Target PC: enable Wake-on-LAN in BIOS and in the NIC driver.

## HTTP API

| Endpoint | Auth | Description |
|---|---|---|
| `GET /wake?key=<HTTP_KEY>` | key | Broadcast a magic packet for the target MAC |
| `GET /health` | — | JSON status: RSSI, heap, uptime, reset reason, UDP/wake counters |
| `GET /log` | — | Last 30 persisted events (reset causes, outage summaries) with timestamps |
| `GET /reboot?key=<HTTP_KEY>` | key | Remote clean restart |

Note: only UDP 9 needs to be forwarded on the modem. The HTTP API is LAN-only unless you also forward port 80 (not recommended).

## Diagnosing an outage

When the device goes silent, check in this order:

1. **healthchecks.io alert time + last pings** — RSSI trending down before death → signal problem; sudden stop with healthy telemetry → power or network event.
2. **`/log` after recovery** — `up x47` means it self-healed after 47 boot retries (~35 min offline); `brownout` means power supply trouble; a `net_dead` series means zombie connections or an ISP outage.
3. **Nothing new in `/log` and still dead** — code isn't running at all: power, bootloader strap pin, or hardware. Check the serial monitor (115200 baud).
