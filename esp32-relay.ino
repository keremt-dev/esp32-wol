// ESP32 WoL Relay - yerel agdaki uyuyan PC'yi internetten uyandirmak icin.
//
// Calisma mantigi (mevcut kurulumu bozmaz):
//   Android WoL app (4G) -> your-host.duckdns.org:<DIS_PORT> -> modem WAN
//     -> modem forward (dis port != ic port olabilir!) -> ESP32 (192.168.0.5):9
//     -> ESP yerel broadcast <aktif subnet>.255:9 -> uyuyan PC uyanir
//
// Modem ayari (Gelismis -> Yonlendirme): UDP, Dis <DIS_PORT> (orn. 9999) -> Lokal 192.168.0.5:9
// DIKKAT: Uyandirma istegini DIS porta gonder (orn. duckdns:9999), ic porta (9) degil.
//
// Loglama: healthchecks.io'ya 5 dk'da bir durum ping'i (15 dk susarsa alarm + son ping'lerde
// rssi/heap gidisati) + NVS'te kalici olay halkasi (/log) -> arizanin WiFi'siz kismi da kayitli.
//
// Arduino IDE: Board = "ESP32 Dev Module".
// secrets.example.h dosyasini secrets.h olarak kopyala ve WiFi/DuckDNS bilgilerini oraya yaz.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <string.h>
#include <Preferences.h>
#include <time.h>
#include "esp_system.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy secrets.example.h to secrets.h and fill WiFi, HTTP_KEY, DuckDNS, healthchecks and target MAC values."
#endif

IPAddress staticIP(192,168,0,5);    // ESP sabit IP (modem DHCP'si .10'dan basliyor, .5 bos)
IPAddress gateway (192,168,0,1);
IPAddress subnet  (255,255,255,0);
IPAddress dnsSrv  (8,8,8,8);        // public DNS (modem DNS proxy guvenilmez -> duckdns cozulemiyordu)
IPAddress dns2    (1,1,1,1);

IPAddress broadcastIP(192,168,0,255);
const uint16_t WOL_PORT = 9;

// targetMac secrets.h'te tanimli (public repo'da gercek MAC yayinlamamak icin)

// DuckDNS: ESP 7/24 acik oldugu icin WAN IP degisse de hostname'i guncel tutar
unsigned long lastDuck = 0;
const unsigned long DUCK_INTERVAL = 300000UL;   // 5 dakika
int duckLastCode = 0;             // son deneme HTTP kodu (-1 = baglanti acilamadi)
bool duckLastOk = false;          // son deneme "OK" dondu mu (/health'te gorunur)
unsigned long lastDuckOkMs = 0;   // son basarili guncellemenin zamani

WiFiUDP udpIn, udpOut;
WebServer http(80);

bool wifiConfigOk = false;
bool udpInOk = false;
bool udpOutOk = false;
bool watchdogOk = false;

unsigned long udpPackets = 0;
unsigned long udpAccepted = 0;
unsigned long udpRejected = 0;
unsigned long udpSelfIgnored = 0;
unsigned long httpWakeCount = 0;
unsigned long lastUdpMs = 0;
unsigned long lastHttpWakeMs = 0;
int lastUdpLen = 0;
bool lastUdpMagic = false;
IPAddress lastUdpRemote(0,0,0,0);

// esp_reset_reason() planli ESP.restart() sebeplerini ayirt etmez; hepsi genelde "sw" gorunur.
// DIKKAT: RTC_DATA_ATTR yazilimsal resette SIFIRLANIR (yalnizca deep-sleep uyanisinda korunur) --
// sahada "restartCause hep unknown, bootCount hep 1" olarak yakalandi. RTC_NOINIT_ATTR ise
// sw/wdt/brownout resetlerinde kalir; ilk aciliste copluk icerdigi icin magic sayi ile dogrulanir.
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR char lastRestartCause[24];
RTC_NOINIT_ATTR uint32_t bootCount;        // guc kesilmedikce her acilista artar
RTC_NOINIT_ATTR uint32_t wifiRetryCount;   // ardarda wifi_boot_timeout: NVS yerine RTC'de sayilir (flash asinmasin)
RTC_NOINIT_ATTR uint32_t netDeadCount;     // ardarda net_dead restart (ISS kesintisinde backoff icin)
const uint32_t RTC_MAGIC = 0x574F4C32;

void rtcInit(){
  if(rtcMagic != RTC_MAGIC){   // poweron / ilk flash: RTC icerigi gecersiz -> temizle
    rtcMagic = RTC_MAGIC;
    strcpy(lastRestartCause, "unknown");
    bootCount = 0; wifiRetryCount = 0; netDeadCount = 0;
  }
}

void setRestartCause(const char* cause){
  strncpy(lastRestartCause, cause, sizeof(lastRestartCause) - 1);
  lastRestartCause[sizeof(lastRestartCause) - 1] = '\0';
}

// ===== KALICI OLAY KAYDI (NVS) =====
// Arizanin WiFi'siz gecen kismini da kaydeder: son LOG_CAP olay flash'ta tutulur,
// elektrik kesilse bile silinmez, cihaz ayaga kalkinca /log ile okunur.
Preferences logStore;
const int LOG_CAP = 30;
struct LogEvent { uint32_t boot; uint32_t epoch; uint32_t ms; int16_t rssi; uint16_t heapKB; char ev[16]; };
struct LogRing  { uint8_t count; uint8_t head; uint8_t _pad[2]; LogEvent e[LOG_CAP]; };
LogRing logs;

// bootCount / wifiRetryCount / netDeadCount yukarida RTC_NOINIT_ATTR olarak tanimli

void logLoad(){
  logStore.begin("wollog", false);
  if(logStore.getBytes("ring", &logs, sizeof(logs)) != sizeof(logs) || logs.count > LOG_CAP || logs.head >= LOG_CAP)
    memset(&logs, 0, sizeof(logs));
}

void logEvent(const char* ev){
  LogEvent& e = logs.e[logs.head];
  e.boot = bootCount; e.ms = millis();
  time_t now = time(nullptr);
  e.epoch = (now > 1700000000) ? (uint32_t)now : 0;   // NTP henuz senkron degilse 0 -> boot+ms ile siralanir
  e.rssi = (WiFi.status()==WL_CONNECTED) ? (int16_t)WiFi.RSSI() : 0;
  e.heapKB = (uint16_t)(ESP.getFreeHeap() / 1024);
  strncpy(e.ev, ev, sizeof(e.ev)-1); e.ev[sizeof(e.ev)-1] = '\0';
  logs.head = (logs.head + 1) % LOG_CAP; if(logs.count < LOG_CAP) logs.count++;
  logStore.putBytes("ring", &logs, sizeof(logs));
  Serial.println(String("[LOG] ") + ev);
}

// ===== HEALTHCHECKS.IO DEAD-MAN'S-SWITCH =====
// 5 dk'da bir durum ping'i; healthchecks 15 dk ping gelmezse alarm uretir. Boylece olum ani ve
// oncesindeki rssi/heap gidisati cihazin DISINDA kayitli olur. Ping ayni zamanda gercek uctan-uca
// baglanti testidir: WL_CONNECTED "bagli" yalani soylese de (zombi baglanti) ping soylemez.
unsigned long lastHc = 0;
const unsigned long HC_INTERVAL = 300000UL;   // 5 dk (healthchecks'te period=5dk, grace=10dk ayarla)
int hcFails = 0;

bool hcPing(const String& msg){
  if(WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient req;
  if(!req.begin(client, HC_PING_URL)) return false;
  req.setConnectTimeout(5000); req.setTimeout(5000);
  int code = req.POST(msg);
  req.end();
  return code == 200;
}

void restartNow(const char* cause){
  setRestartCause(cause);
  // wifi_boot_timeout dongusu RTC sayaciyla ozetlenir, net_dead cagrildigi yerde kisitli loglanir (NVS asinmasin)
  if(strcmp(cause,"wifi_boot_timeout") != 0 && strcmp(cause,"net_dead") != 0){
    logEvent(cause);
    hcPing(String("restart: ") + cause);   // son nefes: planli restart sebebi disarida da gorunsun
  }
  delay(200);
  ESP.restart();
}

// ===== LOOP WATCHDOG =====
// loop() her tur lastLoopMs'i besler. Eger loop() WDT_TIMEOUT_MS boyunca ilerlemezse
// (orn. http.handleClient() bayat sokette takilirsa), bu task -ayri cekirdekte calistigi icin-
// donmus loop'tan etkilenmez ve cipi resetler. Bu gercek ESP-IDF task WDT degil; loop canlilik kontroludur.
volatile unsigned long lastLoopMs = 0;
const unsigned long WDT_TIMEOUT_MS = 30000;   // 30 sn beslenmezse reset
void watchdogTask(void* pv){
  for(;;){
    if(lastLoopMs != 0 && (millis() - lastLoopMs) > WDT_TIMEOUT_MS){
      Serial.println("\n[WDT] loop 30sn dondu -> restart");
      setRestartCause("loop_wdt");
      delay(50);
      esp_restart();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// CORS + Connection:close -> her yanitta soketi kapat (bayat-soket birikmesine karsi ilk savunma)
void cors(){ http.sendHeader("Access-Control-Allow-Origin","*"); http.sendHeader("Connection","close"); }

// Son reset sebebi (/health ve boot log icin) -> arizadan sonra "neden resetlendi" gorunur
const char* resetReasonStr(){
  switch(esp_reset_reason()){
    case ESP_RST_POWERON:  return "poweron";   // fis cek-tak / ilk acilis
    case ESP_RST_SW:       return "sw";         // esp_restart()/ESP.restart() (watchdog, 24s, wifi-drop, low-heap)
    case ESP_RST_PANIC:    return "panic";      // kod crash
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";   // yetersiz guc!
    case ESP_RST_EXT:      return "ext";
    default:               return "other";
  }
}

void broadcastBytes(const uint8_t* d, int len){
  udpOut.beginPacket(broadcastIP, WOL_PORT); udpOut.write(d, len); udpOut.endPacket();
}

void updateBroadcastIP(){
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  broadcastIP = IPAddress(
    ip[0] | (uint8_t)~mask[0],
    ip[1] | (uint8_t)~mask[1],
    ip[2] | (uint8_t)~mask[2],
    ip[3] | (uint8_t)~mask[3]
  );
  Serial.print("Broadcast IP: "); Serial.println(broadcastIP);
}

bool isMagicForTarget(const uint8_t* buf, int len){
  if(len < 102) return false;
  for(int i=0;i<6;i++){
    if(buf[i] != 0xFF) return false;
  }
  for(int r=0;r<16;r++){
    if(memcmp(buf + 6 + r * 6, targetMac, 6) != 0) return false;
  }
  return true;
}

void sendMagic(const uint8_t* mac){
  uint8_t p[102]; memset(p,0xFF,6);
  for(int i=0;i<16;i++) memcpy(p+6+i*6, mac, 6);
  broadcastBytes(p,102); Serial.println("-> Magic packet broadcast (" + broadcastIP.toString() + ":9)");
}

void handleWake(){
  cors();
  if(http.arg("key")!=HTTP_KEY){ http.send(401,"text/plain","unauthorized"); return; }
  httpWakeCount++; lastHttpWakeMs = millis();
  sendMagic(targetMac); http.send(200,"application/json","{\"status\":\"sent\"}");
}

void handleReboot(){   // uzaktan manuel reset (sadece ESP saglikliyken calisir)
  cors();
  if(http.arg("key")!=HTTP_KEY){ http.send(401,"text/plain","unauthorized"); return; }
  http.send(200,"application/json","{\"status\":\"rebooting\"}");
  delay(300); restartNow("manual");
}

void handleHealth(){   // web arayuzunun "Role (ESP32)" gostergesi icin
  cors();
  bool ok = WiFi.status()==WL_CONNECTED && wifiConfigOk && udpInOk && udpOutOk && watchdogOk;
  String j = String("{\"ok\":") + (ok ? "true" : "false") +
             ",\"ssid\":\"" + WiFi.SSID() +
             "\",\"ip\":\"" + WiFi.localIP().toString() +
             "\",\"gateway\":\"" + WiFi.gatewayIP().toString() +
             "\",\"subnet\":\"" + WiFi.subnetMask().toString() +
             "\",\"broadcast\":\"" + broadcastIP.toString() +
             "\",\"rssi\":" + String((long)WiFi.RSSI()) +
             ",\"uptime\":" + String((unsigned long)(millis()/1000)) +
             ",\"reset\":\"" + resetReasonStr() + "\"" +
             ",\"restartCause\":\"" + String(lastRestartCause) + "\"" +
             ",\"wifiConfig\":" + (wifiConfigOk ? "true" : "false") +
             ",\"udpIn\":" + (udpInOk ? "true" : "false") +
             ",\"udpOut\":" + (udpOutOk ? "true" : "false") +
             ",\"watchdog\":" + (watchdogOk ? "true" : "false") +
             ",\"bootCount\":" + String(bootCount) +
             ",\"duckOk\":" + (duckLastOk ? "true" : "false") +
             ",\"duckCode\":" + String(duckLastCode) +
             ",\"msSinceDuckOk\":" + String(lastDuckOkMs == 0 ? 0 : millis() - lastDuckOkMs) +
             ",\"udpPackets\":" + String(udpPackets) +
             ",\"udpAccepted\":" + String(udpAccepted) +
             ",\"udpRejected\":" + String(udpRejected) +
             ",\"udpSelfIgnored\":" + String(udpSelfIgnored) +
             ",\"lastUdpRemote\":\"" + lastUdpRemote.toString() + "\"" +
             ",\"lastUdpLen\":" + String(lastUdpLen) +
             ",\"lastUdpMagic\":" + (lastUdpMagic ? "true" : "false") +
             ",\"msSinceLastUdp\":" + String(lastUdpMs == 0 ? 0 : millis() - lastUdpMs) +
             ",\"httpWakeCount\":" + String(httpWakeCount) +
             ",\"msSinceLastHttpWake\":" + String(lastHttpWakeMs == 0 ? 0 : millis() - lastHttpWakeMs) +
             ",\"heap\":" + String((unsigned long)ESP.getFreeHeap()) +
             ",\"minHeap\":" + String((unsigned long)ESP.getMinFreeHeap()) +
             ",\"maxBlock\":" + String((unsigned long)ESP.getMaxAllocHeap()) + "}";
  http.send(200,"application/json", j);
}

void handleLog(){   // NVS'teki son olaylar (en yeniden en eskiye) - ariza sonrasi otopsi icin
  cors();
  String j = String("{\"bootCount\":") + bootCount + ",\"wifiRetry\":" + wifiRetryCount +
             ",\"netDead\":" + netDeadCount + ",\"events\":[";
  for(int i = 0; i < logs.count; i++){
    int idx = (logs.head - 1 - i + LOG_CAP) % LOG_CAP;
    LogEvent& e = logs.e[idx];
    char ts[24] = "";
    if(e.epoch){ time_t t = e.epoch; struct tm tmv; localtime_r(&t, &tmv); strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv); }
    if(i) j += ",";
    j += String("{\"boot\":") + e.boot + ",\"time\":\"" + ts + "\",\"ms\":" + e.ms +
         ",\"ev\":\"" + e.ev + "\",\"rssi\":" + e.rssi + ",\"heapKB\":" + e.heapKB + "}";
  }
  j += "]}";
  http.send(200, "application/json", j);
}

void updateDuckDNS(){
  lastDuck = millis();
  if(WiFi.status()!=WL_CONNECTED) return;
  WiFiClient client;                 // ESP32'de HTTPS sorun cikariyorsa DuckDNS HTTP de OK donuyor
  HTTPClient req;
  String url = String("http://www.duckdns.org/update?domains=") + DUCKDNS_DOMAIN +
               "&token=" + DUCKDNS_TOKEN + "&ip=";  // ip bos -> DuckDNS WAN IP'yi otomatik algilar
  if(req.begin(client, url)){
    req.setConnectTimeout(5000); req.setTimeout(5000);   // DuckDNS takilirsa loop'u uzun bloklamasin
    int code = req.GET();
    String body = req.getString();
    duckLastCode = code;
    duckLastOk = (code == 200 && body.startsWith("OK"));
    if(duckLastOk) lastDuckOkMs = millis();
    Serial.println("DuckDNS: HTTP " + String(code) + " -> " + body); // "OK" beklenir
    req.end();
  } else { duckLastCode = -1; duckLastOk = false; Serial.println("DuckDNS: baglanti acilamadi"); }
}

void connectWiFi(){
  WiFi.persistent(false);              // kimlik bilgisini her begin'de NVS'e yazma -> flash asinmasini onler
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  wifiConfigOk = WiFi.config(staticIP,gateway,subnet,dnsSrv,dns2);
  if(!wifiConfigOk) Serial.println("WiFi.config basarisiz -> statik IP/DNS uygulanmamis olabilir");
  WiFi.begin(WIFI_SSID,WIFI_PASS); Serial.print("WiFi baglaniyor");
  unsigned long t0 = millis();
  while(WiFi.status()!=WL_CONNECTED){
    delay(400); Serial.print(".");
    // Elektrik kesintisi sonrasi modem henuz hazir degilse: 45sn'de bir TEMIZ yeniden dene.
    // (Erken WiFi.begin takili kalabiliyor; restart ile her seferinde temiz association.)
    if(millis()-t0 > 45000){ Serial.println("\n45sn baglanamadi (modem hazir degil?) -> ESP restart"); wifiRetryCount++; restartNow("wifi_boot_timeout"); }
  }
  WiFi.setSleep(false);                // modem-sleep KAPALI: 7/24 duyarli, "islevsel olu" baglanti riski azalir
  configTime(3*3600, 0, "pool.ntp.org", "time.google.com");   // TR saati: olay kayitlarina gercek zaman damgasi
  // "up x47" = 47 deneme (~35 dk) suren baglanti kesintisinden sonra ayaga kalkti demektir
  String upEv = wifiRetryCount ? (String("up x") + wifiRetryCount) : String("up");
  logEvent(upEv.c_str());
  wifiRetryCount = 0;
  updateBroadcastIP();
  Serial.print("\nESP IP: "); Serial.println(WiFi.localIP());
}

void setup(){
  Serial.begin(115200);
  rtcInit();                           // RTC_NOINIT alanlarini dogrula (ilk aciliste copluk icerir)
  bootCount++;
  logLoad();                           // NVS olay halkasini yukle (logEvent'ten ONCE sart)
  esp_reset_reason_t bootReason = esp_reset_reason();
  if(bootReason == ESP_RST_POWERON) setRestartCause("poweron");
  else if(bootReason != ESP_RST_SW) setRestartCause("unplanned");
  // Planli olmayan resetler (poweron/brownout/panic) dis kaynakli ve nadir -> her zaman kayda deger.
  // Planli (sw) resetler zaten restartNow icinde loglandi.
  if(bootReason != ESP_RST_SW) logEvent(resetReasonStr());
  Serial.println(String("\nBoot. Son reset sebebi: ") + resetReasonStr() + ", planli sebep: " + lastRestartCause);
  connectWiFi();
  udpInOk = udpIn.begin(WOL_PORT);
  udpOutOk = udpOut.begin(40000);
  if(!udpInOk) Serial.println("UDP dinleme baslatilamadi (port 9)");
  if(!udpOutOk) Serial.println("UDP cikis soketi baslatilamadi");
  http.on("/wake", handleWake);
  http.on("/health", handleHealth);
  http.on("/reboot", handleReboot);
  http.on("/log", handleLog);
  http.on("/", handleHealth);
  http.begin();
  Serial.println("WoL relay hazir: UDP:9 dinleniyor + HTTP /wake + /health + /log aktif");
  updateDuckDNS();   // acilista hemen guncelle
  // Acilis ping'i: "cihaz geri geldi" sinyali + reset sebebi healthchecks gecmisine yazilsin
  hcPing(String("boot: reset=") + resetReasonStr() + " cause=" + lastRestartCause +
         " boot#" + bootCount + " rssi=" + WiFi.RSSI());
  lastHc = millis() - HC_INTERVAL/2;   // hc ile DuckDNS ayni tura denk gelmesin (2.5dk faz farki)
  // Loop watchdog'u baslat (0. cekirdek; loop 1. cekirdekte). loop donsa bile bu calisir.
  lastLoopMs = millis();
  watchdogOk = (xTaskCreatePinnedToCore(watchdogTask, "wdt", 4096, NULL, 1, NULL, 0) == pdPASS);
  Serial.println(watchdogOk ? "Loop watchdog aktif (30sn)" : "Loop watchdog baslatilamadi");
}

void loop(){
  lastLoopMs = millis();   // watchdog'u besle: loop ilerliyor demektir
  int n = udpIn.parsePacket();
  if(n>0){
    udpPackets++;
    lastUdpMs = millis();
    lastUdpRemote = udpIn.remoteIP();
    if(lastUdpRemote!=WiFi.localIP()){            // kendi broadcast'ini tekrar relay etme
      uint8_t buf[256]; int len = udpIn.read(buf,sizeof(buf));
      lastUdpLen = len;
      lastUdpMagic = isMagicForTarget(buf,len);
      if(lastUdpMagic){
        udpAccepted++;
        sendMagic(targetMac);
        Serial.println("-> Dogrulanmis magic packet relay edildi");
      } else {
        udpRejected++;
        Serial.println("UDP paket reddedildi: hedef MAC magic packet degil");
      }
    } else {
      udpSelfIgnored++;
      udpIn.flush();
    }
  }
  http.handleClient();
  // WiFi koptuysa (orn. modem reboot): yeniden baglan, sonra TEMIZ soketler icin ESP'yi resetle.
  // Sadece reconnect yetmiyor -> WebServer/UDP soketleri reconnect sonrasi bayatlayip donuyor.
  if(WiFi.status()!=WL_CONNECTED){
    delay(3000);                                     // gecici blip ise gec
    if(WiFi.status()!=WL_CONNECTED){
      Serial.println("WiFi koptu -> yeniden baglanip ESP resetlenecek (temiz soketler)");
      WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS);
      unsigned long t0 = millis();
      while(WiFi.status()!=WL_CONNECTED && millis()-t0 < 60000){ delay(500); Serial.print("."); }
      delay(1000);
      restartNow("wifi_drop");                       // baglandi da baglanamadi da: temiz baslangic
    }
  }
  if(millis() - lastDuck >= DUCK_INTERVAL) updateDuckDNS();   // periyodik DuckDNS guncelle
  // Periyodik durum ping'i + zombi baglanti dedektoru
  if(millis() - lastHc >= HC_INTERVAL){
    lastHc = millis();
    String body = String("up=") + (millis()/1000) + "s boot#" + bootCount + " rssi=" + WiFi.RSSI() +
                  " heap=" + ESP.getFreeHeap() + " maxBlock=" + ESP.getMaxAllocHeap() +
                  " cause=" + lastRestartCause + " duck=" + (duckLastOk ? "ok" : "fail");
    if(hcPing(body)){ hcFails = 0; netDeadCount = 0; }
    else {
      hcFails++;
      Serial.println("hc-ping basarisiz #" + String(hcFails));
      // WiFi "bagli" gorunse de trafik akmiyor olabilir (zombi baglanti) -> temiz restart tek care.
      // Surekli kesintide (ISS/DNS coktu) 15dk yerine 30dk'da bir dene ki bosuna donup durmasin.
      int limit = (netDeadCount >= 3) ? 6 : 3;
      if(hcFails >= limit){
        netDeadCount++;
        if(netDeadCount == 1 || netDeadCount % 10 == 0) logEvent("net_dead");  // uzun kesinti ring'i doldurmasin
        restartNow("net_dead");
      }
    }
  }
  if(millis() >= 86400000UL){ Serial.println("24s doldu -> rutin restart"); restartNow("daily"); } // gunluk guvence
  if(ESP.getMaxAllocHeap() < 20000){ Serial.println("Dusuk heap (fragmentasyon) -> temiz restart"); restartNow("low_heap"); } // bellek cokmeden once
}




