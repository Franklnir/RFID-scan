/* ============================================================================
 * ESP32-S3 Mini — PZEM004T v3 + BME280/BH1750 + Supabase (REST) + Relay + Tarif
 * Versi: Pure Supabase (tanpa Firebase)
 * Update:
 *  - Interval kirim 2 detik
 *  - WebServer lokal dengan kontrol relay real-time
 *  - Fallback SoftAP IP statik saat WiFi rumah mati
 *  - Static IP opsional untuk jaringan rumah
 *
 * Fitur:
 * - Akurasi PZEM tinggi (trimmed mean 5x, blend Pm & Pcalc, auto-zero I, auto-cal energi 2 titik)
 * - BME280 + BH1750 dengan I2C Guard (auto-recover)
 * - Relay 4 channel (active low) dengan RelayGuard (debounce + rate limit) + scheduler harian
 * - Supabase:
 *   • monitoring_log  : simpan riwayat + biaya listrik (harian & bulanan)
 *   • relay_channel   : status relay + jadwal
 *   • device_commands : remote command (reset_kwh, calibrate, vi_ref, meter_push)
 *   • tariff_config   : konfigurasi tarif listrik (harga/kWh, beban, pajak)
 * - LED notif: nyala 1 detik tiap kali data berhasil dikirim ke Supabase
 * - WebServer:
 *   • /      : dashboard lokal dengan kontrol relay real-time
 *   • /json  : data JSON untuk realtime
 *   • /relay : endpoint untuk kontrol relay via POST
 * ========================================================================== */

#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>

#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>
#include <PZEM004Tv30.h>

#include <Preferences.h>
#include <ArduinoJson.h>

// ================== KONFIG WIFI ==================
#define WIFI_SSID_1      "GEORGIA"
#define WIFI_PASSWORD_1  "Georgia12345"
#define WIFI_SSID_2      "Universitas Pelita Bangsa New"
#define WIFI_PASSWORD_2  "megah123"

// ====== Static IP untuk SSID_1 (rumah) ======
#define USE_STATIC_IP_HOME  1

#if USE_STATIC_IP_HOME
  IPAddress HOME_LOCAL_IP(192, 168, 1, 70);
  IPAddress HOME_GATEWAY (192, 168, 1, 1);
  IPAddress HOME_SUBNET  (255, 255, 255, 0);
  IPAddress HOME_DNS1    (8, 8, 8, 8);
  IPAddress HOME_DNS2    (1, 1, 1, 1);
#endif

// ====== SoftAP fallback ======
#define AP_FALLBACK_SSID   "ESP32-S3-Monitoring-AP"
#define AP_FALLBACK_PASS   "12345678"   // minimal 8 char
IPAddress AP_IP     (192, 168, 1, 100);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET (255, 255, 255, 0);

// ================== KONFIG SUPABASE ==================
// GANTI DENGAN URL & API KEY PUNYAMU
#define SUPABASE_URL      "https://eacdwtdcpecxxeebfcxx.supabase.co"
#define SUPABASE_API_KEY  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImVhY2R3dGRjcGVjeHhlZWJmY3h4Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjUwMjg4ODIsImV4cCI6MjA4MDYwNDg4Mn0.ywHK7DBkinOb9SRx0beUaFHhJabEvE0Nw5Q9ONlEvjY"

#define DEVICE_ID         "ESP32-S3-Monitoring-01"
#define TABLE_LOG         "monitoring_log"
#define TABLE_RELAY       "relay_channel"
#define TABLE_CMD         "device_commands"
#define TABLE_TARIFF      "tariff_config"

// ================== WAKTU (WIB) ==================
const long TZ_OFFSET = 7 * 3600; // UTC+7
const int  DST_OFFSET = 0;
const char* ntpServers[] = {
  "id.pool.ntp.org","pool.ntp.org","time.google.com","time.nist.gov"
};

// ================== UART PZEM ==================
#define PZEM_RX_PIN 11
#define PZEM_TX_PIN 12
HardwareSerial PZEMSerial(1);
PZEM004Tv30 pzem(PZEMSerial, PZEM_RX_PIN, PZEM_TX_PIN);

// ================== Sensor Lingkungan & IO ==================
#define SDA_PIN    9
#define SCL_PIN    8
#define BUZZER_PIN 4
#define LED_PIN    13

Adafruit_BME280 bme;
BH1750 lightMeter;

// ================== Relay (ACTIVE-LOW) ==================
const int relayPins[4] = {6, 7, 5, 10};
bool lastRelayState[4] = {false, false, false, false};
const char* RELAY_LABELS[4] = {"Relay 1", "Relay 2", "Relay 3", "Relay 4"};
String RELAY_DESCRIPTIONS[4] = {"Penerangan", "AC/Exhaust", "Pompa Air", "Cadangan"};

// ================== Filter EMA ==================
const float ALPHA = 0.25f;
struct Filtered {
  float v=NAN, i=NAN, p=NAN, e=NAN, f=NAN, pf=NAN, S=NAN, Q=NAN;
} filt;
static inline float ema(float prev, float x){
  if(isnan(prev)) return x;
  return prev + ALPHA*(x-prev);
}

// ================== Akumulator Energi & Kalibrasi ==================
Preferences prefs;
float today_kwh=0.0f, month_kwh=0.0f, lastE_kwh=NAN;
int lastDay=-1, lastMonth=-1, lastYear=-1;

struct Calib {
  float v_gain = 1.000f, v_off = 0.000f;
  float i_gain = 1.000f, i_off = 0.000f;
  float p_gain = 1.000f, p_off = 0.000f;
  float kwh_gain = 1.000f;
} calib;

void loadCalib() {
  prefs.begin("pzem-calib", true);
  calib.v_gain = prefs.getFloat("v_gain", 1.0f);
  calib.v_off  = prefs.getFloat("v_off",  0.0f);
  calib.i_gain = prefs.getFloat("i_gain", 1.0f);
  calib.i_off  = prefs.getFloat("i_off",  0.0f);
  calib.p_gain = prefs.getFloat("p_gain", 1.0f);
  calib.p_off  = prefs.getFloat("p_off",  0.0f);
  calib.kwh_gain = prefs.getFloat("kwh_gain", 1.0f);
  prefs.end();
}
void saveCalib() {
  prefs.begin("pzem-calib", false);
  prefs.putFloat("v_gain", calib.v_gain);
  prefs.putFloat("v_off",  calib.v_off);
  prefs.putFloat("i_gain", calib.i_gain);
  prefs.putFloat("i_off",  calib.i_off);
  prefs.putFloat("p_gain", calib.p_gain);
  prefs.putFloat("p_off",  calib.p_off);
  prefs.putFloat("kwh_gain", calib.kwh_gain);
  prefs.end();
}

// ================== Tarif Listrik (dari Supabase) ==================
struct Tariff {
  String id;
  float harga_per_kwh = 1438.0f;
  float biaya_beban   = 0.0f;
  float pajak_persen  = 6.0f;
} tarif;

// ================== Timer ==================
unsigned long lastSendMs=0;
// Kirim ke Supabase tiap 2 detik
const unsigned long SEND_INTERVAL_MS=2000;

unsigned long lastPrint=0;
const unsigned long PRINT_EVERY_MS=1000;

// ================== Scheduler ==================
unsigned long lastScheduleTick = 0;
const unsigned long TICK_MS = 1000;
String lastMinuteChecked = "";

// ================== WebServer ==================
WebServer server(80);
bool apFallbackStarted = false;

// ================== WiFi Status Tracking ==================
bool wasConnected = false;
unsigned long lastWifiCheckMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000; // 30 detik untuk coba reconnect
unsigned long wifiDisconnectedTime = 0;

// ================== Util waktu ==================
static bool isLeap(int y){ return ((y%4==0)&&(y%100!=0))||(y%400==0); }
static long daysBeforeYear(int y){
  long d=0; for(int yr=1970; yr<y; ++yr) d+=365+(isLeap(yr)?1:0); return d;
}
static int  daysBeforeMonth(int y,int m0){
  static const int cum[12]={0,31,59,90,120,151,181,212,243,273,304,334};
  int d=cum[m0]; if(m0>=2&&isLeap(y)) d+=1; return d;
}
static time_t timegm_portable(const struct tm* tmv){
  int y=tmv->tm_year+1900, m=tmv->tm_mon, d=tmv->tm_mday;
  long long days=(long long)daysBeforeYear(y)+daysBeforeMonth(y,m)+(d-1);
  return (time_t)(days*86400LL + tmv->tm_hour*3600LL + tmv->tm_min*60LL + tmv->tm_sec);
}
static bool myGetLocalTime(struct tm* info, uint32_t ms=5000){
  time_t now; uint32_t t0=millis();
  while((millis()-t0)<=ms){
    time(&now);
    if(now>1609459200){ localtime_r(&now, info); return true; }
    delay(10); yield();
  }
  return false;
}

bool syncTimeFromHTTP(){
  WiFiClient client; const char* host="google.com";
  if(!client.connect(host,80)) return false;
  client.print(String("HEAD / HTTP/1.1\r\nHost: ") + host + "\r\nConnection: close\r\n\r\n");
  uint32_t t0=millis();
  while(client.connected() && millis()-t0<3000){
    String line=client.readStringUntil('\n');
    if(line.startsWith("Date: ")){
      struct tm tmv{}; char wk[4],mon[4]; int d,y,H,M,S;
      if(sscanf(line.c_str(),"Date: %3s, %d %3s %d %d:%d:%d GMT",wk,&d,mon,&y,&H,&M,&S)==7){
        const char* months="JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* p=strstr(months,mon); int m=p?((int)(p-months)/3):0;
        tmv.tm_year=y-1900; tmv.tm_mon=m; tmv.tm_mday=d; tmv.tm_hour=H; tmv.tm_min=M; tmv.tm_sec=S;
        time_t tutc=timegm_portable(&tmv); struct timeval now={tutc+TZ_OFFSET,0}; settimeofday(&now,nullptr);
        client.stop(); return true;
      }
    }
    if(line=="\r") break;
  }
  client.stop(); return false;
}

static inline void ensureTime(){
  Serial.print("Sinkronisasi waktu");
  bool ok=false;
  for(size_t i=0;i<sizeof(ntpServers)/sizeof(ntpServers[0]);++i){
    configTime(TZ_OFFSET, DST_OFFSET, ntpServers[i]);
    struct tm ti; unsigned long t0=millis();
    while(!myGetLocalTime(&ti,250) && (millis()-t0<3500)){
      Serial.print("."); delay(250);
    }
    if(myGetLocalTime(&ti,1)){ ok=true; break; }
  }
  if(!ok){
    Serial.print("... fallback HTTP time");
    if(syncTimeFromHTTP()) ok=true;
  }
  Serial.println(ok? " -> OK":" -> GAGAL");
}

bool timeReady(){
  time_t now=time(nullptr);
  return now > 24*3600;
}

String tsWIB(){
  time_t now=time(nullptr); struct tm tm_info;
  localtime_r(&now, &tm_info);
  char buf[32]; strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M:%S",&tm_info);
  return String(buf);
}
String isoTimeWIB(){
  time_t now=time(nullptr); struct tm tm_info;
  localtime_r(&now,&tm_info);
  char buf[32]; strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%S+07:00",&tm_info);
  return String(buf);
}
String now_HHMM(){
  struct tm t; if(!myGetLocalTime(&t)) return "";
  char b[6]; snprintf(b,sizeof(b),"%02d:%02d",t.tm_hour,t.tm_min);
  return String(b);
}
int nowMinutes(){
  struct tm t; if(!myGetLocalTime(&t)) return -1;
  return t.tm_hour*60 + t.tm_min;
}
int dayIndex(){
  struct tm t; if(!myGetLocalTime(&t)) return 0;
  return t.tm_wday; // 0=Sun..6=Sat
}
bool parseHHMM_toMinutes(const String& hhmm, int& outMin){
  if(hhmm.length()<5) return false;
  int hh = hhmm.substring(0,2).toInt();
  int mm = hhmm.substring(3,5).toInt();
  if(hh<0||hh>23||mm<0||mm>59) return false;
  outMin = hh*60 + mm; return true;
}
bool isInWindow(const String& startHHMM, const String& endHHMM, int nowMin){
  int s,e; if(!parseHHMM_toMinutes(startHHMM, s) || !parseHHMM_toMinutes(endHHMM, e)) return true;
  if(s==e) return true;
  if(s<e)  return (nowMin >= s && nowMin < e);
  return (nowMin >= s) || (nowMin < e);
}

// ================== Util cetak ==================
static inline void printDetailedReadout(float V,float I,float P,float E,float F,float PF,float S,float Q,const char* ts){
  Serial.printf("[%s] V=%.2fV I=%.3fA P=%.1fW S=%.1fVA Q=%.1fvar PF=%.3f F=%.2fHz E=%.5fkWh\n",
    ts, V,I,P,S,Q,PF,F,E);
}

// ================== NVS helpers energi bulanan ==================
void loadMonthBaseline(){
  prefs.begin("pzem-bill", true);
  month_kwh = prefs.getFloat("month_kwh", 0.0f);
  lastYear  = prefs.getInt("year", -1);
  lastMonth = prefs.getInt("month",-1);
  prefs.end();
}
void saveMonthBaseline(float mkwh, int y, int m){
  prefs.begin("pzem-bill", false);
  prefs.putFloat("month_kwh", mkwh);
  prefs.putInt("year", y);
  prefs.putInt("month", m);
  prefs.end();
}

// ================== WiFi connect strategy ==================
static bool waitWiFiConnected(uint32_t ms){
  uint32_t t0 = millis();
  while(millis()-t0 < ms){
    if(WiFi.status()==WL_CONNECTED) return true;
    delay(150);
  }
  return false;
}

// reset config ke DHCP
static void clearStaticConfig(){
  WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0));
}

bool connectWiFiPriority(){
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  Serial.printf("🔌 Coba WiFi utama: %s\n", WIFI_SSID_1);

#if USE_STATIC_IP_HOME
  WiFi.config(HOME_LOCAL_IP, HOME_GATEWAY, HOME_SUBNET, HOME_DNS1, HOME_DNS2);
#endif

  WiFi.begin(WIFI_SSID_1, WIFI_PASSWORD_1);
  if(waitWiFiConnected(12000)){
    Serial.printf("✅ WiFi utama OK | SSID=%s | IP=%s | RSSI=%d dBm\n",
      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  Serial.println("⚠ WiFi utama gagal, fallback SSID 2 (DHCP)...");
  WiFi.disconnect(true, true);
  delay(200);

  clearStaticConfig();
  WiFi.begin(WIFI_SSID_2, WIFI_PASSWORD_2);
  if(waitWiFiConnected(12000)){
    Serial.printf("✅ WiFi fallback OK | SSID=%s | IP=%s | RSSI=%d dBm\n",
      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  Serial.println("❌ Semua WiFi gagal.");
  return false;
}

// ================== SoftAP fallback ==================
void startAPFallback(){
  if(apFallbackStarted) return;

  Serial.println("🛜 Mengaktifkan SoftAP fallback...");

  WiFi.mode(WIFI_AP_STA); // tetap boleh coba reconnect STA
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  bool ok = WiFi.softAP(AP_FALLBACK_SSID, AP_FALLBACK_PASS);

  if(ok){
    apFallbackStarted = true;
    Serial.printf("✅ SoftAP ON | SSID=%s | IP=%s\n",
      AP_FALLBACK_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("❌ SoftAP gagal aktif.");
  }
}

void stopAPFallback(){
  if(!apFallbackStarted) return;
  
  Serial.println("🛜 Mematikan SoftAP fallback...");
  WiFi.softAPdisconnect(true);
  apFallbackStarted = false;
  Serial.println("✅ SoftAP dimatikan.");
}

// ================== RELAY GUARD ==================
struct RelayGuard {
  unsigned long lastChangeMs[4] = {0,0,0,0};
  unsigned long lastAnyChangeMs = 0;
  const unsigned long debounceMs = 120;
  const unsigned long minPerChanGapMs = 180;
  const unsigned long minAnyGapMs    = 120;
  bool wifiJustReconn = false;
  unsigned long wifiReconnAt = 0;

  void markWifiReconn(){ wifiJustReconn = true; wifiReconnAt = millis(); }
  bool holdoffAfterWifi(){
    if(!wifiJustReconn) return false;
    if(millis()-wifiReconnAt>300) {wifiJustReconn=false; return false;}
    return true;
  }
  bool canToggle(int idx){
    unsigned long now = millis();
    if (holdoffAfterWifi()) return false;
    if (now - lastAnyChangeMs < minAnyGapMs) return false;
    if (now - lastChangeMs[idx] < max(debounceMs,minPerChanGapMs)) return false;
    return true;
  }
  void apply(int idx, bool on){
    if(idx<0||idx>=4) return;
    if(lastRelayState[idx]==on) return;
    if(!canToggle(idx)) return;
    unsigned long sinceAny = millis()-lastAnyChangeMs;
    if (sinceAny < minAnyGapMs) delay(minAnyGapMs - sinceAny);
    lastRelayState[idx]=on;
    digitalWrite(relayPins[idx], on?LOW:HIGH);
    lastChangeMs[idx]=millis();
    lastAnyChangeMs=millis();
    Serial.printf("[RELAY] %s -> %s\n", RELAY_LABELS[idx], on? "ON":"OFF");
    digitalWrite(LED_PIN, HIGH); delay(30); digitalWrite(LED_PIN, LOW);
  }
} rguard;

void applyRelay(int idx, bool on){ rguard.apply(idx,on); }

// ================== Statistik PZEM ==================
struct PZEMRead {
  float v=NAN,i=NAN,p=NAN,e=NAN,f=NAN,pf=NAN; bool ok=false;
};

static float trimmedMean3of5(float a, float b, float c, float d, float e) {
  float arr[5] = {a, b, c, d, e};
  for (int i = 1; i < 5; i++) {
    float key = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j] > key) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
  return (arr[1] + arr[2] + arr[3]) / 3.0f;
}

PZEMRead robustReadPZEM5(uint16_t gap_ms=80){
  float v[5], i[5], p[5], e[5], f[5], pf[5];
  for(int k=0;k<5;k++){
    v[k]=pzem.voltage();
    i[k]=pzem.current();
    p[k]=pzem.power();
    e[k]=pzem.energy();
    f[k]=pzem.frequency();
    pf[k]=pzem.pf();
    delay(gap_ms);
  }
  PZEMRead R;
  R.v  = trimmedMean3of5(v[0],v[1],v[2],v[3],v[4]);
  R.i  = trimmedMean3of5(i[0],i[1],i[2],i[3],i[4]);
  R.p  = trimmedMean3of5(p[0],p[1],p[2],p[3],p[4]);
  R.e  = trimmedMean3of5(e[0],e[1],e[2],e[3],e[4]);
  R.f  = trimmedMean3of5(f[0],f[1],f[2],f[3],f[4]);
  R.pf = trimmedMean3of5(pf[0],pf[1],pf[2],pf[3],pf[4]);

  R.ok = !(isnan(R.v)||isnan(R.i)||isnan(R.p)||isnan(R.e)||isnan(R.f)||isnan(R.pf));
  return R;
}

// ================== Auto-zero & AutoCal Energi ==================
const float ZERO_I_MAX_A = 0.10f;
const uint32_t ZERO_WINDOW_MS = 6000;
const float I_OFF_MIN = -0.20f;
const float I_OFF_MAX =  0.00f;
uint32_t zeroStartMs = 0; bool zeroWindowActive = false;

static inline bool allRelaysOff(){
  for(int i=0;i<4;i++) if (lastRelayState[i]) return false;
  return true;
}
void autoZeroCurrentIfIdle(float i_inst) {
  if (allRelaysOff() && !isnan(i_inst) && fabsf(i_inst) <= ZERO_I_MAX_A) {
    if (!zeroWindowActive) {
      zeroWindowActive = true;
      zeroStartMs = millis();
    }
    if (zeroWindowActive && (millis() - zeroStartMs >= ZERO_WINDOW_MS)) {
      float new_i_off = constrain(-i_inst, I_OFF_MIN, I_OFF_MAX);
      if (fabsf(new_i_off - calib.i_off) > 0.002f) {
        calib.i_off = new_i_off;
        saveCalib();
        Serial.printf("🤖 Auto-zero: set i_off = %.3f A (idle=%.3f A)\n", calib.i_off, i_inst);
      }
      zeroWindowActive = false;
    }
  } else {
    zeroWindowActive = false;
  }
}

// --- Referensi meter PLN (dua titik) ---
struct MeterRef { double kwh = NAN; uint32_t ts = 0; };
MeterRef refA, refB; double snapA_e = NAN, snapB_e = NAN;

void loadMeterRefs(){
  prefs.begin("meter-ref", true);
  refA.kwh = prefs.getDouble("A_kwh", NAN);
  refA.ts  = prefs.getUInt("A_ts", 0);
  refB.kwh = prefs.getDouble("B_kwh", NAN);
  refB.ts  = prefs.getUInt("B_ts", 0);
  snapA_e  = prefs.getDouble("A_e", NAN);
  snapB_e  = prefs.getDouble("B_e", NAN);
  prefs.end();
}
void saveMeterRefs(){
  prefs.begin("meter-ref", false);
  prefs.putDouble("A_kwh", refA.kwh);
  prefs.putUInt("A_ts", refA.ts);
  prefs.putDouble("B_kwh", refB.kwh);
  prefs.putUInt("B_ts", refB.ts);
  prefs.putDouble("A_e", snapA_e);
  prefs.putDouble("B_e", snapB_e);
  prefs.end();
}

void tryApplyEnergyGainFromRefs(){
  if (isnan(refA.kwh) || isnan(refB.kwh) || isnan(snapA_e) || isnan(snapB_e)) return;
  if (refA.ts <= refB.ts) return;

  const uint32_t MIN_CALIB_SECONDS = 3600 * 6;
  if (refA.ts - refB.ts < MIN_CALIB_SECONDS) {
    Serial.println("⚠ Auto-Calib: Jeda waktu antar referensi terlalu singkat (<6 jam), kalibrasi diabaikan.");
    return;
  }

  double dRef  = refA.kwh  - refB.kwh;
  double dPzem = snapA_e   - snapB_e;

  const double MIN_ENERGY_DELTA_KWH = 0.5;
  if (dRef <= MIN_ENERGY_DELTA_KWH || dPzem <= MIN_ENERGY_DELTA_KWH) {
    Serial.println("⚠ Auto-Calib: Delta energi terlalu kecil (<0.5 kWh), kalibrasi diabaikan.");
    return;
  }
  if (dPzem < 0) {
    Serial.println("⚠ PZEM energy snapshot mundur — abaikan kalibrasi.");
    return;
  }

  double gain = dRef / dPzem;
  if (gain < 0.90) gain = 0.90;
  if (gain > 1.10) gain = 1.10;

  calib.p_gain   *= (float)gain;
  calib.kwh_gain *= (float)gain;
  saveCalib();

  Serial.printf("🤖 Auto-tune energi: dRef=%.3f kWh dPzem=%.3f kWh -> gain=%.4f\n", dRef, dPzem, gain);
  Serial.printf("    p_gain=%.4f  kwh_gain=%.4f (tersimpan)\n", calib.p_gain, calib.kwh_gain);

  refB.kwh = NAN;
  snapB_e  = NAN;
  saveMeterRefs();
}

void handleMeterPush(double kwhRef, uint32_t tsRef){
  if (isnan(kwhRef) || tsRef == 0) return;
  refB = refA;
  snapB_e = snapA_e;
  refA.kwh = kwhRef;
  refA.ts  = tsRef;
  snapA_e  = pzem.energy();
  saveMeterRefs();
  Serial.printf("📥 MeterRef: kWh=%.3f ts=%u | snapshot E=%.5f kWh\n",
      refA.kwh, refA.ts, snapA_e);
  tryApplyEnergyGainFromRefs();
}

// ================== I2C GUARD & ENV SENSORS ==================
struct I2CGuard {
  bool scanAddr(uint8_t addr){
    Wire.beginTransmission(addr);
    return (Wire.endTransmission()==0);
  }
  void busRecover(){
    pinMode(SDA_PIN, INPUT_PULLUP);
    pinMode(SCL_PIN, INPUT_PULLUP);
    for(int i=0;i<9;i++){
      pinMode(SCL_PIN, OUTPUT); delayMicroseconds(5);
      digitalWrite(SCL_PIN, LOW); delayMicroseconds(5);
      pinMode(SCL_PIN, INPUT_PULLUP); delayMicroseconds(5);
    }
    pinMode(SDA_PIN, OUTPUT); digitalWrite(SDA_PIN, HIGH); delayMicroseconds(5);
    pinMode(SCL_PIN, OUTPUT); digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5);
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000);
    Wire.setTimeOut(50);
  }
} i2cguard;

struct EnvSensors {
  bool bmeOK=false, bhOK=false;
  uint8_t bmeAddr=0x76;
  unsigned failCnt=0;
  unsigned long lastGoodMs=0, lastInitMs=0;
  const unsigned maxFail=8;
  const unsigned long reinitEveryMs=600000;
  const unsigned long staleMs=120000;

  bool begin(){
    lastInitMs=millis();
    if(!i2cguard.scanAddr(bmeAddr)){
      Serial.println("I2C: BME280 tak terdeteksi, recovery bus...");
      i2cguard.busRecover();
    }
    bmeOK = bme.begin(bmeAddr);
    if(!bmeOK){
      bmeAddr=0x77;
      if(!i2cguard.scanAddr(bmeAddr)) i2cguard.busRecover();
      bmeOK = bme.begin(bmeAddr);
    }
    bhOK  = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    if(!bhOK){
      lightMeter.configure(BH1750::CONTINUOUS_HIGH_RES_MODE);
      bhOK = i2cguard.scanAddr(0x23) || i2cguard.scanAddr(0x5C);
    }
    Serial.printf("BME280 : %s (addr 0x%02X)\n", bmeOK?"OK":"FAIL", bmeAddr);
    Serial.printf("BH1750 : %s\n", bhOK?"OK":"FAIL");
    failCnt=0;
    return (bmeOK||bhOK);
  }
  void autoRecover(){
    unsigned long now=millis();
    if (failCnt>=maxFail){
      Serial.println("I2C: gagal beruntun — RECOVER bus & reinit sensors");
      i2cguard.busRecover(); begin();
    } else if ((now-lastInitMs)>reinitEveryMs){
      Serial.println("I2C: reinit berkala (prevent lock-up)");
      begin();
    } else if (lastGoodMs>0 && (now-lastGoodMs)>staleMs){
      Serial.println("I2C: bacaan stale — reinit");
      begin();
    }
  }
  bool read(float &t, float &h, float &p_hpa, float &alt, float &lux){
    bool any=false, ok=true;
    if (bmeOK){
      float tt = bme.readTemperature();
      float hh = bme.readHumidity();
      float pp = bme.readPressure();
      if (isnan(tt) || isnan(hh) || isnan(pp)) { ok=false; failCnt++; }
      else { t=tt; h=hh; p_hpa=pp/100.0f; alt=bme.readAltitude(1013.25f); any=true; }
    }
    if (bhOK){
      float lx = lightMeter.readLightLevel();
      if (isnan(lx) || lx<0) { ok=false; failCnt++; }
      else { lux=lx; any=true; }
    }
    if (ok && any){
      failCnt=0; lastGoodMs=millis();
    }
    if (!ok) autoRecover();
    return any;
  }
} env;

// ================== SUPABASE: Helper umum ==================
void addSupabaseAuthHeaders(HTTPClient &https){
  https.addHeader("apikey", SUPABASE_API_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_API_KEY);
}

// ====== SUPABASE: Ambil tarif aktif ======
bool fetchActiveTariffFromSupabase() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ Tariff: WiFi belum terhubung.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_TARIFF +
               "?active=eq.true&order=updated_at.desc&limit=1";

  if (!https.begin(client, url)) {
    Serial.println("❌ Tariff fetch: gagal begin().");
    return false;
  }

  addSupabaseAuthHeaders(https);
  https.addHeader("Accept", "application/json");

  int code = https.GET();
  if (code != 200) {
    Serial.printf("⚠ Tariff fetch HTTP: %d\n", code);
    String body = https.getString();
    Serial.println(body);
    https.end();
    return false;
  }

  String resp = https.getString();
  https.end();

  DynamicJsonDocument doc(2048);
  auto err = deserializeJson(doc, resp);
  if (err) {
    Serial.print("❌ Tariff JSON parse: ");
    Serial.println(err.c_str());
    return false;
  }

  if (!doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
    Serial.println("⚠ Tariff: tidak ada baris active, pakai default.");
    return false;
  }

  JsonObject row = doc.as<JsonArray>()[0];

  const char* id = row["id"] | "";
  tarif.id            = id ? String(id) : "";
  tarif.harga_per_kwh = (float)(row["harga_per_kwh"] | tarif.harga_per_kwh);
  tarif.biaya_beban   = (float)(row["biaya_beban"]   | tarif.biaya_beban);
  tarif.pajak_persen  = (float)(row["pajak_persen"]  | tarif.pajak_persen);

  Serial.println("✅ Tarif aktif dari Supabase:");
  Serial.printf("   id=%s\n", tarif.id.c_str());
  Serial.printf("   harga_per_kwh=%.2f  beban/bulan=%.2f  pajak=%.2f%%\n",
                tarif.harga_per_kwh, tarif.biaya_beban, tarif.pajak_persen);
  return true;
}

// ================== SUPABASE: Kirim monitoring_log ==================
bool sendToSupabaseLog(
  const String& tsISO,
  float suhu, float kelembapan, float tekanan,
  float altitude, float lightLevel
){
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ Supabase Log: WiFi tidak terhubung, skip.");
    return false;
  }

  int nonNan = 0;
  if(!isnan(filt.v)) nonNan++;
  if(!isnan(filt.i)) nonNan++;
  if(!isnan(filt.p)) nonNan++;
  if(!isnan(filt.S)) nonNan++;
  if(!isnan(filt.Q)) nonNan++;
  if(!isnan(filt.pf)) nonNan++;
  if(!isnan(filt.f)) nonNan++;
  if(!isnan(suhu)) nonNan++;
  if(!isnan(kelembapan)) nonNan++;
  if(!isnan(tekanan)) nonNan++;
  if(!isnan(altitude)) nonNan++;
  if(!isnan(lightLevel)) nonNan++;
  if(nonNan == 0) {
    Serial.println("⚠ Supabase Log: semua nilai NaN, tidak dikirim.");
    return false;
  }

  // ===== HITUNG BIAYA LISTRIK =====
  float energiHarian  = today_kwh;
  float energiBulanan = month_kwh;

  float biayaEnergiHarian  = energiHarian  * tarif.harga_per_kwh;
  float biayaEnergiBulanan = energiBulanan * tarif.harga_per_kwh;

  float bebanBulanan = tarif.biaya_beban;
  float bebanHarian  = (tarif.biaya_beban > 0.0f) ? (tarif.biaya_beban / 30.0f) : 0.0f;

  float dppHarian  = biayaEnergiHarian  + bebanHarian;
  float dppBulanan = biayaEnergiBulanan + bebanBulanan;

  float pajakHarian  = dppHarian  * (tarif.pajak_persen / 100.0f);
  float pajakBulanan = dppBulanan * (tarif.pajak_persen / 100.0f);

  float totalHarian  = dppHarian  + pajakHarian;
  float totalBulanan = dppBulanan + pajakBulanan;

  float ppjHarian   = pajakHarian * 0.5f;
  float pbjtHarian  = pajakHarian * 0.5f;
  float ppjBulanan  = pajakBulanan * 0.5f;
  float pbjtBulanan = pajakBulanan * 0.5f;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_LOG;

  if (!https.begin(client, url)) {
    Serial.println("❌ Supabase Log: gagal begin() HTTPClient.");
    return false;
  }

  addSupabaseAuthHeaders(https);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\"";
  payload += ",\"ts\":\"" + tsISO + "\"";

  if(!isnan(filt.v))  payload += ",\"tegangan_v\":"        + String(filt.v,3);
  if(!isnan(filt.i))  payload += ",\"arus_a\":"            + String(filt.i,4);
  if(!isnan(filt.p))  payload += ",\"daya_aktif_w\":"      + String(filt.p,3);
  if(!isnan(filt.S))  payload += ",\"daya_semu_va\":"      + String(filt.S,3);
  if(!isnan(filt.Q))  payload += ",\"daya_reaktif_var\":"  + String(filt.Q,3);
  if(!isnan(filt.f))  payload += ",\"frekuensi_hz\":"      + String(filt.f,3);
  if(!isnan(filt.pf)) payload += ",\"faktor_daya\":"       + String(filt.pf,4);

  float E_total = pzem.energy();
  if(!isnan(E_total))   payload += ",\"energi_total_kwh\":"   + String(E_total,5);
  payload += ",\"energi_harian_kwh\":"  + String(today_kwh,5);
  payload += ",\"energi_bulanan_kwh\":" + String(month_kwh,5);

  if(!isnan(suhu))       payload += ",\"suhu_c\":"         + String(suhu,2);
  if(!isnan(kelembapan)) payload += ",\"kelembapan_rh\":"  + String(kelembapan,2);
  if(!isnan(tekanan))    payload += ",\"tekanan_hpa\":"    + String(tekanan,2);
  if(!isnan(altitude))   payload += ",\"altitude_m\":"     + String(altitude,2);
  if(!isnan(lightLevel)) payload += ",\"light_level_lux\":"+ String(lightLevel,2);

  payload += ",\"wifi_rssi\":" + String(WiFi.RSSI());

  if (tarif.id.length()) {
    payload += ",\"tarif_id\":\"" + tarif.id + "\"";
  }
  payload += ",\"tarif_harga_per_kwh\":" + String(tarif.harga_per_kwh,2);
  payload += ",\"tarif_biaya_beban\":"   + String(tarif.biaya_beban,2);
  payload += ",\"tarif_pajak_persen\":"  + String(tarif.pajak_persen,2);

  payload += ",\"biaya_energi_harian_rp\":" + String(biayaEnergiHarian,2);
  payload += ",\"beban_harian_rp\":"        + String(bebanHarian,2);
  payload += ",\"ppj_harian_rp\":"          + String(ppjHarian,2);
  payload += ",\"pbjt_harian_rp\":"         + String(pbjtHarian,2);
  payload += ",\"total_harian_rp\":"        + String(totalHarian,2);

  payload += ",\"biaya_energi_bulanan_rp\":" + String(biayaEnergiBulanan,2);
  payload += ",\"beban_bulanan_rp\":"        + String(bebanBulanan,2);
  payload += ",\"ppj_bulanan_rp\":"          + String(ppjBulanan,2);
  payload += ",\"pbjt_bulanan_rp\":"         + String(pbjtBulanan,2);
  payload += ",\"total_bulanan_rp\":"        + String(totalBulanan,2);

  payload += "}";

  int httpCode = https.POST(payload);
  bool ok = false;

  if (httpCode > 0) {
    if (httpCode == 201 || httpCode == 200 || httpCode == 204) {
      ok = true;
    } else {
      String resp = https.getString();
      Serial.printf("Respon Supabase Log (%d): %s\n", httpCode, resp.c_str());
    }
  } else {
    Serial.printf("❌ Supabase Log POST gagal, error: %s\n",
                  https.errorToString(httpCode).c_str());
  }

  https.end();
  return ok;
}

// ================== SUPABASE: Relay ==================
struct RelayConfig {
  bool valid = false;
  bool state = false;
  String waktuOn;
  String waktuOff;
  bool day[7];
};
RelayConfig relayCfg[4];
unsigned long lastRelayFetchMs = 0;
const unsigned long RELAY_FETCH_INTERVAL_MS = 10000;

bool upsertRelayStateSupabase(int ch, bool on, const char* byTag){
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_RELAY +
               "?on_conflict=device_id,channel";

  if (!https.begin(client, url)){
    return false;
  }

  addSupabaseAuthHeaders(https);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");

  String payload = "[{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\"";
  payload += ",\"channel\":" + String(ch);
  payload += ",\"state\":" + String(on ? "true":"false");
  payload += ",\"meta_by\":\"" + String(byTag) + "\"";
  payload += ",\"meta_ts\":\"" + isoTimeWIB() + "\"";
  payload += "}]";

  int code = https.POST(payload);
  bool ok = (code==201 || code==200 || code==204);
  https.end();
  return ok;
}

void scheduleSetRelay(int idx, bool on, const char* byTag){
  applyRelay(idx,on);
  upsertRelayStateSupabase(idx,on,byTag);
}

bool initRelayDefaultsInSupabase(){
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_RELAY +
               "?on_conflict=device_id,channel";

  if (!https.begin(client, url)){
    return false;
  }
  addSupabaseAuthHeaders(https);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");

  String payload = "[";
  for (int ch=0; ch<4; ++ch){
    if (ch>0) payload += ",";
    payload += "{";
    payload += "\"device_id\":\"" + String(DEVICE_ID) + "\"";
    payload += ",\"channel\":" + String(ch);
    payload += ",\"state\":false";
    payload += ",\"sun\":true,\"mon\":true,\"tue\":true,\"wed\":true,\"thu\":true,\"fri\":true,\"sat\":true";
    payload += "}";
  }
  payload += "]";

  int code = https.POST(payload);
  bool ok = (code==201 || code==200 || code==204);
  https.end();
  return ok;
}

void fetchRelayConfigFromSupabase(){
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastRelayFetchMs < RELAY_FETCH_INTERVAL_MS) return;
  lastRelayFetchMs = millis();

  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_RELAY +
               "?device_id=eq." + DEVICE_ID + "&select=*";

  if (!https.begin(client, url)){
    return;
  }
  addSupabaseAuthHeaders(https);
  https.addHeader("Accept", "application/json");

  int code = https.GET();
  if (code != 200){
    https.end();
    return;
  }

  String resp = https.getString();
  https.end();

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, resp)) return;
  if (!doc.is<JsonArray>()) return;

  for(int i=0;i<4;i++) relayCfg[i].valid = false;

  JsonArray arr = doc.as<JsonArray>();
  if (arr.size()==0){
    initRelayDefaultsInSupabase();
    return;
  }

  for(JsonObject row : arr){
    int ch = row["channel"] | -1;
    if (ch<0 || ch>3) continue;

    RelayConfig &cfg = relayCfg[ch];
    cfg.valid = true;

    bool dbState = row["state"] | false;
    cfg.state = dbState;

    if (dbState != lastRelayState[ch]){
      applyRelay(ch, dbState);
    }

    const char* won  = row["waktu_on"].isNull()  ? nullptr : row["waktu_on"].as<const char*>();
    const char* woff = row["waktu_off"].isNull() ? nullptr : row["waktu_off"].as<const char*>();

    cfg.waktuOn  = won  ? String(won).substring(0,5)  : "";
    cfg.waktuOff = woff ? String(woff).substring(0,5) : "";

    cfg.day[0] = row["sun"].isNull() ? true : row["sun"].as<bool>();
    cfg.day[1] = row["mon"].isNull() ? true : row["mon"].as<bool>();
    cfg.day[2] = row["tue"].isNull() ? true : row["tue"].as<bool>();
    cfg.day[3] = row["wed"].isNull() ? true : row["wed"].as<bool>();
    cfg.day[4] = row["thu"].isNull() ? true : row["thu"].as<bool>();
    cfg.day[5] = row["fri"].isNull() ? true : row["fri"].as<bool>();
    cfg.day[6] = row["sat"].isNull() ? true : row["sat"].as<bool>();
  }
}

// ================== SUPABASE: Device Commands ==================
unsigned long lastCmdPollMs = 0;
const unsigned long CMD_POLL_INTERVAL_MS = 5000;

bool markCommandProcessed(long id){
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_CMD +
               "?id=eq." + String(id);

  if (!https.begin(client, url)){
    return false;
  }
  addSupabaseAuthHeaders(https);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  String payload = "{\"processed\":true,\"processed_at\":\"" + isoTimeWIB() + "\"}";
  int code = https.PATCH(payload);
  bool ok = (code==200 || code==204);
  https.end();
  return ok;
}

void resetEnergyCountersLocal(){
  Serial.println("\n!!! ================================= !!!");
  Serial.println("!!!      RESET ENERGI PZEM + LOKAL   !!!");
  Serial.println("!!! ================================= !!!");

  bool resetOK = pzem.resetEnergy();
  Serial.printf("PZEM resetEnergy() -> %s\n", resetOK ? "OK" : "GAGAL");

  today_kwh  = 0.0f;
  month_kwh  = 0.0f;
  lastE_kwh  = 0.0f;

  float currentEnergy = pzem.energy();
  if (!isnan(currentEnergy)){
    lastE_kwh = currentEnergy;
    Serial.printf("Baseline energi baru: %.5f kWh\n", lastE_kwh);
  } else {
    lastE_kwh = 0.0f;
  }

  if (timeReady()){
    time_t nowt=time(nullptr); struct tm tm_info;
    localtime_r(&nowt,&tm_info);
    saveMonthBaseline(0.0f, tm_info.tm_year+1900, tm_info.tm_mon+1);
  }
}

void handleCalibrateCommand(JsonObject cmd){
  double v_gain   = cmd["v_gain"]   | NAN;
  double v_off    = cmd["v_off"]    | NAN;
  double i_gain   = cmd["i_gain"]   | NAN;
  double i_off    = cmd["i_off"]    | NAN;
  double p_gain   = cmd["p_gain"]   | NAN;
  double p_off    = cmd["p_off"]    | NAN;
  double kwh_gain = cmd["kwh_gain"] | NAN;

  if (!isnan(v_gain))   calib.v_gain   = (float)v_gain;
  if (!isnan(v_off))    calib.v_off    = (float)v_off;
  if (!isnan(i_gain))   calib.i_gain   = (float)i_gain;
  if (!isnan(i_off))    calib.i_off    = (float)i_off;
  if (!isnan(p_gain))   calib.p_gain   = (float)p_gain;
  if (!isnan(p_off))    calib.p_off    = (float)p_off;
  if (!isnan(kwh_gain)) calib.kwh_gain = (float)kwh_gain;

  saveCalib();
}

void handleViRefCommand(JsonObject cmd){
  double v_ref = cmd["v_ref"] | NAN;
  double i_ref = cmd["i_ref"] | NAN;

  PZEMRead r = robustReadPZEM5(60);

  if (!isnan(v_ref) && v_ref>80 && v_ref<280 && !isnan(r.v) && r.v>50){
    float new_gain = (float)(v_ref / r.v);
    calib.v_gain = constrain(new_gain, 0.90f, 1.10f);
  }
  if (!isnan(i_ref) && i_ref>=0 && i_ref<100 && !isnan(r.i) && r.i>=0){
    float new_gain = (r.i>0.01f)? (float)(i_ref / r.i) : calib.i_gain;
    calib.i_gain = constrain(new_gain, 0.90f, 1.20f);
  }
  saveCalib();
}

void pollDeviceCommandsSupabase(){
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastCmdPollMs < CMD_POLL_INTERVAL_MS) return;
  lastCmdPollMs = millis();

  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  String url = String(SUPABASE_URL) + "/rest/v1/" + TABLE_CMD
               + "?device_id=eq." + DEVICE_ID
               + "&processed=is.false&order=created_at.asc&limit=8";

  if (!https.begin(client, url)){
    return;
  }
  addSupabaseAuthHeaders(https);
  https.addHeader("Accept", "application/json");

  int code = https.GET();
  if (code != 200){
    https.end();
    return;
  }

  String resp = https.getString();
  https.end();

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, resp)) return;
  if (!doc.is<JsonArray>()) return;

  JsonArray arr = doc.as<JsonArray>();
  if (arr.size()==0) return;

  for (JsonObject cmd : arr){
    long id = cmd["id"] | 0;
    const char* type = cmd["cmd_type"] | "";
    if (id<=0 || type[0]=='\0') continue;

    if (strcmp(type,"reset_kwh")==0){
      resetEnergyCountersLocal();
    } else if (strcmp(type,"calibrate")==0){
      handleCalibrateCommand(cmd);
    } else if (strcmp(type,"vi_ref")==0){
      handleViRefCommand(cmd);
    } else if (strcmp(type,"meter_push")==0){
      double kwhRef = cmd["meter_kwh_ref"] | NAN;
      long tsRef    = cmd["meter_ts"]      | 0;
      handleMeterPush(kwhRef, (uint32_t)tsRef);
    }

    markCommandProcessed(id);
  }
}

// ================== WEB: JSON builder ==================
String buildJsonSnapshot(float suhu, float kelembapan, float tekanan, float altitude, float lux){
  DynamicJsonDocument doc(2048);

  doc["device_id"] = DEVICE_ID;
  doc["ts"] = isoTimeWIB();

  doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["wifi_ssid"] = WiFi.SSID();
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["ip_sta"] = WiFi.localIP().toString();
  doc["ip_ap"]  = WiFi.softAPIP().toString();

  // PZEM data - gunakan isnan() untuk mengecek dan set nilai null jika NaN
  JsonObject pzemObj = doc.createNestedObject("pzem");
  if(!isnan(filt.v))  pzemObj["v"] = filt.v;
  if(!isnan(filt.i))  pzemObj["i"] = filt.i;
  if(!isnan(filt.p))  pzemObj["p"] = filt.p;
  if(!isnan(filt.S))  pzemObj["S"] = filt.S;
  if(!isnan(filt.Q))  pzemObj["Q"] = filt.Q;
  if(!isnan(filt.pf)) pzemObj["pf"] = filt.pf;
  if(!isnan(filt.f))  pzemObj["f"] = filt.f;
  
  float E_total = pzem.energy();
  if(!isnan(E_total)) pzemObj["E_total"] = E_total;

  JsonObject energy = doc.createNestedObject("energy");
  energy["today_kwh"] = today_kwh;
  energy["month_kwh"] = month_kwh;
  
  JsonObject calibObj = doc.createNestedObject("calib");
  calibObj["kwh_gain"] = calib.kwh_gain;

  JsonObject envObj = doc.createNestedObject("env");
  if(!isnan(suhu))       envObj["suhu_c"] = suhu;
  if(!isnan(kelembapan)) envObj["hum_rh"] = kelembapan;
  if(!isnan(tekanan))    envObj["press_hpa"] = tekanan;
  if(!isnan(altitude))   envObj["alt_m"] = altitude;
  if(!isnan(lux))        envObj["lux"] = lux;

  JsonObject tarifObj = doc.createNestedObject("tarif");
  tarifObj["id"] = tarif.id;
  tarifObj["harga_per_kwh"] = tarif.harga_per_kwh;
  tarifObj["biaya_beban"] = tarif.biaya_beban;
  tarifObj["pajak_persen"] = tarif.pajak_persen;

  JsonArray rel = doc.createNestedArray("relay");
  for(int i = 0; i < 4; i++){
    JsonObject r = rel.createNestedObject();
    r["channel"] = i;
    r["label"] = RELAY_LABELS[i];
    r["description"] = RELAY_DESCRIPTIONS[i];
    r["state"] = lastRelayState[i];
    r["valid_cfg"] = relayCfg[i].valid;
    r["waktu_on"] = relayCfg[i].waktuOn;
    r["waktu_off"] = relayCfg[i].waktuOff;
    // Tambahkan jadwal untuk hari ini
    int dIdx = dayIndex();
    r["today_enabled"] = relayCfg[i].valid ? relayCfg[i].day[dIdx] : true;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// ================== WEB: HTML page ==================
// ================== WEB: HTML page ==================
String buildHtmlPage(){
  // Enhanced HTML dengan kontrol relay real-time
  String html;
  html.reserve(8000);

  html += F("<!doctype html><html><head>");
  html += F("<meta charset='utf-8'/>");
  html += F("<meta name='viewport' content='width=device-width, initial-scale=1'/>");
  html += F("<title>ESP32-S3 Monitoring & Control</title>");
  html += F("<style>");
  html += F("body{font-family:system-ui,Arial;margin:0;background:#0b1020;color:#e8ecff;}");
  html += F(".wrap{padding:18px;max-width:1080px;margin:auto;}");
  html += F(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px;}");
  html += F(".card{background:#131a33;border:1px solid #22305a;border-radius:14px;padding:14px;}");
  html += F("h1{font-size:20px;margin:0 0 10px 0;}");
  html += F("h2{font-size:14px;margin:0 0 8px 0;opacity:.9;}");
  html += F(".kv{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px dashed #22305a;}");
  html += F(".kv:last-child{border-bottom:none;}");
  html += F(".small{font-size:12px;opacity:.8;}");
  html += F(".badge{display:inline-block;padding:2px 8px;border-radius:999px;background:#1f2a55;font-size:11px;}");
  html += F(".relay-card{border-left:4px solid #3498db;margin:8px 0;padding:8px;background:#151e3a;}");
  html += F(".relay-header{display:flex;justify-content:space-between;align-items:center;}");
  html += F(".relay-controls{display:flex;gap:8px;}");
  html += F(".relay-btn{padding:6px 12px;border:none;border-radius:6px;cursor:pointer;font-weight:bold;}");
  html += F(".relay-on{background:#27ae60;color:white;}");
  html += F(".relay-off{background:#e74c3c;color:white;}");
  html += F(".relay-toggle{background:#3498db;color:white;}");
  html += F(".relay-btn:disabled{opacity:0.5;cursor:not-allowed;}");
  html += F(".status-on{color:#27ae60;}");
  html += F(".status-off{color:#e74c3c;}");
  html += F(".schedule-info{font-size:11px;margin-top:4px;color:#95a5a6;}");
  html += F(".pzem-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:6px;}");
  html += F("@media(max-width:760px){.grid{grid-template-columns:1fr;}.pzem-grid{grid-template-columns:1fr;}}");
  html += F("</style>");
  html += F("</head><body><div class='wrap'>");
  html += F("<h1>ESP32-S3 Monitoring & Control <span class='badge' id='modeBadge'>—</span></h1>");
  html += F("<div class='small' id='ts'>—</div>");

  html += F("<div class='grid' style='margin-top:12px;'>");

  // Network
  html += F("<div class='card'><h2>Network</h2>");
  html += F("<div class='kv'><span>Status</span><b id='wifiStatus'>—</b></div>");
  html += F("<div class='kv'><span>SSID</span><b id='ssid'>—</b></div>");
  html += F("<div class='kv'><span>RSSI</span><b id='rssi'>—</b></div>");
  html += F("<div class='kv'><span>IP STA</span><b id='ipSta'>—</b></div>");
  html += F("<div class='kv'><span>IP AP</span><b id='ipAp'>—</b></div>");
  html += F("</div>");

  // PZEM
  html += F("<div class='card'><h2>PZEM004T</h2>");
  html += F("<div class='pzem-grid'>");
  html += F("<div class='kv'><span>Tegangan</span><b id='v'>—</b></div>");
  html += F("<div class='kv'><span>Arus</span><b id='i'>—</b></div>");
  html += F("<div class='kv'><span>Daya Aktif</span><b id='p'>—</b></div>");
  html += F("<div class='kv'><span>Daya Semu</span><b id='S'>—</b></div>");
  html += F("<div class='kv'><span>Daya Reaktif</span><b id='Q'>—</b></div>");
  html += F("<div class='kv'><span>PF</span><b id='pf'>—</b></div>");
  html += F("<div class='kv'><span>Frekuensi</span><b id='f'>—</b></div>");
  html += F("<div class='kv'><span>Energi Total</span><b id='E'>—</b></div>");
  html += F("</div></div>");

  // Energy
  html += F("<div class='card'><h2>Energi & Biaya</h2>");
  html += F("<div class='kv'><span>Energi Harian</span><b id='kwhToday'>—</b></div>");
  html += F("<div class='kv'><span>Energi Bulanan</span><b id='kwhMonth'>—</b></div>");
  html += F("<div class='kv'><span>Biaya Harian</span><b id='biayaHarian'>—</b></div>");
  html += F("<div class='kv'><span>Biaya Bulanan</span><b id='biayaBulanan'>—</b></div>");
  html += F("<div class='kv'><span>kWh Gain</span><b id='kwhGain'>—</b></div>");
  html += F("</div>");

  // Env
  html += F("<div class='card'><h2>Lingkungan</h2>");
  html += F("<div class='kv'><span>Suhu</span><b id='t'>—</b></div>");
  html += F("<div class='kv'><span>Kelembapan</span><b id='h'>—</b></div>");
  html += F("<div class='kv'><span>Tekanan</span><b id='pr'>—</b></div>");
  html += F("<div class='kv'><span>Altitude</span><b id='al'>—</b></div>");
  html += F("<div class='kv'><span>Cahaya</span><b id='lx'>—</b></div>");
  html += F("</div>");

  // Tariff
  html += F("<div class='card'><h2>Tarif (aktif)</h2>");
  html += F("<div class='kv'><span>ID</span><b id='tarId'>—</b></div>");
  html += F("<div class='kv'><span>Harga / kWh</span><b id='tarKwh'>—</b></div>");
  html += F("<div class='kv'><span>Beban / bulan</span><b id='tarBeban'>—</b></div>");
  html += F("<div class='kv'><span>Pajak</span><b id='tarPajak'>—</b></div>");
  html += F("</div>");

  // Relay Control Section (FULL WIDTH)
  html += F("<div class='card' style='grid-column:span 2;'><h2>Relay Control</h2>");
  html += F("<div id='relayList'>");
  html += F("   <div style='text-align:center;padding:20px;'>Loading relay data...</div>");
  html += F("</div>");
  html += F("</div>");

  html += F("</div>"); // grid

  html += F("<div class='small' style='margin-top:14px;opacity:.7'>");
  html += F("Tip: Jika WiFi rumah mati, sambungkan HP ke <b>");
  html += AP_FALLBACK_SSID;
  html += F("</b> lalu buka <b>http://192.168.4.1</b>");
  html += F("</div>");

  // JavaScript Section - menggunakan raw string literal untuk menghindari masalah quotes
  html += R"rawliteral(
<script>
let relayStates = [false, false, false, false];
let relayDebounce = false;

async function toggleRelay(channel, newState) {
    if (relayDebounce) return;
    relayDebounce = true;
    
    // Update UI immediately (optimistic update)
    const btnOn = document.getElementById('relayOn_' + channel);
    const btnOff = document.getElementById('relayOff_' + channel);
    const statusEl = document.getElementById('relayStatus_' + channel);
    
    if (btnOn && btnOff && statusEl) {
        btnOn.disabled = true;
        btnOff.disabled = true;
        
        if (newState) {
            btnOn.classList.add('relay-on');
            btnOff.classList.remove('relay-off');
            statusEl.innerHTML = '<span class="status-on">● ON</span>';
            statusEl.className = 'status-on';
        } else {
            btnOn.classList.remove('relay-on');
            btnOff.classList.add('relay-off');
            statusEl.innerHTML = '<span class="status-off">● OFF</span>';
            statusEl.className = 'status-off';
        }
    }
    
    // Send request to server
    try {
        const response = await fetch('/relay', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/x-www-form-urlencoded',
            },
            body: 'channel=' + channel + '&state=' + (newState ? '1' : '0')
        });
        
        if (!response.ok) {
            throw new Error('HTTP error ' + response.status);
        }
        
        const data = await response.json();
        console.log('Relay toggle success:', data);
        
        // Update local state
        relayStates[channel] = newState;
        
    } catch (error) {
        console.error('Relay toggle failed:', error);
        alert('Gagal mengontrol relay: ' + error.message);
        // Revert UI on next tick
        setTimeout(() => tick(), 100);
    } finally {
        setTimeout(() => { relayDebounce = false; }, 300);
    }
}

async function tick(){
  try{
    const r=await fetch('/json',{cache:'no-store'});
    const d=await r.json();
    document.getElementById('ts').textContent=d.ts||'—';
    const wc=!!d.wifi_connected;
    document.getElementById('wifiStatus').textContent=wc?'Connected':'Offline';
    document.getElementById('ssid').textContent=d.wifi_ssid||'—';
    document.getElementById('rssi').textContent=(d.wifi_rssi!=null)?(d.wifi_rssi+' dBm'):'—';
    document.getElementById('ipSta').textContent=d.ip_sta||'—';
    document.getElementById('ipAp').textContent=d.ip_ap||'—';
    document.getElementById('modeBadge').textContent=wc?'STA':'AP';
    
    // PZEM data
    const pz=d.pzem||{};
    document.getElementById('v').textContent=(pz.v!=null)?(pz.v.toFixed(2)+' V'):'—';
    document.getElementById('i').textContent=(pz.i!=null)?(pz.i.toFixed(3)+' A'):'—';
    document.getElementById('p').textContent=(pz.p!=null)?(pz.p.toFixed(1)+' W'):'—';
    document.getElementById('S').textContent=(pz.S!=null)?(pz.S.toFixed(1)+' VA'):'—';
    document.getElementById('Q').textContent=(pz.Q!=null)?(pz.Q.toFixed(1)+' var'):'—';
    document.getElementById('pf').textContent=(pz.pf!=null)?(pz.pf.toFixed(3)):'—';
    document.getElementById('f').textContent=(pz.f!=null)?(pz.f.toFixed(2)+' Hz'):'—';
    document.getElementById('E').textContent=(pz.E_total!=null)?(pz.E_total.toFixed(5)+' kWh'):'—';
    
    // Energy data
    const en=d.energy||{};
    document.getElementById('kwhToday').textContent=(en.today_kwh!=null)?(en.today_kwh.toFixed(3)+' kWh'):'—';
    document.getElementById('kwhMonth').textContent=(en.month_kwh!=null)?(en.month_kwh.toFixed(3)+' kWh'):'—';
    
    // Calculate costs
    const tr=d.tarif||{};
    const todayCost = (en.today_kwh||0) * (tr.harga_per_kwh||0);
    const monthCost = (en.month_kwh||0) * (tr.harga_per_kwh||0);
    document.getElementById('biayaHarian').textContent='Rp ' + todayCost.toFixed(0).replace(/\B(?=(\d{3})+(?!\d))/g, '.');
    document.getElementById('biayaBulanan').textContent='Rp ' + monthCost.toFixed(0).replace(/\B(?=(\d{3})+(?!\d))/g, '.');
    
    // Calibration
    const cb=d.calib||{};
    document.getElementById('kwhGain').textContent=(cb.kwh_gain!=null)?cb.kwh_gain.toFixed(4):'—';
    
    // Environment data
    const ev=d.env||{};
    document.getElementById('t').textContent=(ev.suhu_c!=null)?(ev.suhu_c.toFixed(2)+' °C'):'—';
    document.getElementById('h').textContent=(ev.hum_rh!=null)?(ev.hum_rh.toFixed(2)+' %'):'—';
    document.getElementById('pr').textContent=(ev.press_hpa!=null)?(ev.press_hpa.toFixed(2)+' hPa'):'—';
    document.getElementById('al').textContent=(ev.alt_m!=null)?(ev.alt_m.toFixed(2)+' m'):'—';
    document.getElementById('lx').textContent=(ev.lux!=null)?(ev.lux.toFixed(1)+' lux'):'—';
    
    // Tariff data
    document.getElementById('tarId').textContent=tr.id||'—';
    document.getElementById('tarKwh').textContent=(tr.harga_per_kwh!=null)?('Rp '+Number(tr.harga_per_kwh).toLocaleString('id-ID')+'/kWh'):'—';
    document.getElementById('tarBeban').textContent=(tr.biaya_beban!=null)?('Rp '+Number(tr.biaya_beban).toLocaleString('id-ID')+'/bulan'):'—';
    document.getElementById('tarPajak').textContent=(tr.pajak_persen!=null)?(tr.pajak_persen+' %'):'—';
    
    // Relay data - Build HTML if needed
    const rel=d.relay||[];
    if(rel.length > 0) {
        let out='';
        for(const r of rel){
            const channel = r.channel || 0;
            const state = !!r.state;
            relayStates[channel] = state;
            
            const todayEnabled = r.today_enabled !== false;
            const hasSchedule = r.valid_cfg && (r.waktu_on || r.waktu_off);
            let scheduleText = '';
            if (hasSchedule) {
                scheduleText = '<div class=\'schedule-info\'>Jadwal: ' + (r.waktu_on||'--:--') + ' → ' + (r.waktu_off||'--:--') + ' ' + (todayEnabled?'(Aktif)':'(Nonaktif hari ini)') + '</div>';
            }
            
            out += '<div class=\'relay-card\'>';
            out += '<div class=\'relay-header\'>';
            out += '<div><b>' + (r.label||'Relay '+channel) + '</b><br><small>' + (r.description||'No description') + '</small></div>';
            out += '<div id=\'relayStatus_' + channel + '\' class=\'' + (state?'status-on':'status-off') + '\'>' + (state?'● ON':'● OFF') + '</div>';
            out += '</div>';
            out += scheduleText;
            out += '<div class=\'relay-controls\'>';
            out += '<button id=\'relayOn_' + channel + '\' class=\'relay-btn ' + (state?'relay-on':'') + '\' onclick=\'toggleRelay(' + channel + ', true)\' ' + (!todayEnabled?'disabled':'') + '>ON</button>';
            out += '<button id=\'relayOff_' + channel + '\' class=\'relay-btn ' + (!state?'relay-off':'') + '\' onclick=\'toggleRelay(' + channel + ', false)\' ' + (!todayEnabled?'disabled':'') + '>OFF</button>';
            out += '<button class=\'relay-btn relay-toggle\' onclick=\'toggleRelay(' + channel + ', ' + !state + ')\'>TOGGLE</button>';
            out += '</div>';
            out += '</div>';
        }
        document.getElementById('relayList').innerHTML=out||'—';
    }
  }catch(e){
    console.error('Tick error:', e);
  }
}

// Initial load
tick();
// Update every 1 second
setInterval(tick, 1000);
</script>
)rawliteral";

  html += F("</div></body></html>");
  return html;
}

// ================== WEB: handlers ==================
void handleRoot(){
  server.send(200, "text/html", buildHtmlPage());
}

void handleJson(){
  // ambil snapshot env terbaru cepat
  float suhu=NAN, kelembapan=NAN, tekanan=NAN, altitude=NAN, lux=NAN;
  env.read(suhu, kelembapan, tekanan, altitude, lux);

  String out = buildJsonSnapshot(suhu, kelembapan, tekanan, altitude, lux);
  server.send(200, "application/json", out);
}

void handleRelayControl(){
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  // Parse form data
  String channelStr = server.arg("channel");
  String stateStr = server.arg("state");
  
  if (channelStr.isEmpty() || stateStr.isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"Missing parameters\"}");
    return;
  }
  
  int channel = channelStr.toInt();
  int state = stateStr.toInt();
  
  if (channel < 0 || channel > 3 || (state != 0 && state != 1)) {
    server.send(400, "application/json", "{\"error\":\"Invalid parameters\"}");
    return;
  }
  
  // Apply relay change
  bool newState = (state == 1);
  applyRelay(channel, newState);
  
  // Update Supabase if connected
  if(WiFi.status() == WL_CONNECTED){
    upsertRelayStateSupabase(channel, newState, "web");
  }
  
  // Return JSON response
  String json = "{\"success\":true,\"channel\":" + String(channel) + 
                ",\"state\":" + String(state) + 
                ",\"message\":\"Relay " + String(channel) + " set to " + 
                (newState ? "ON" : "OFF") + "\"}";
  server.send(200, "application/json", json);
  
  Serial.printf("🌐 Web Relay Control: Channel %d -> %s\n", channel, newState ? "ON" : "OFF");
}

void handleRelayStatus(){
  // Return current relay status as JSON
  DynamicJsonDocument doc(512);
  JsonArray relays = doc.createNestedArray("relays");
  
  for (int i = 0; i < 4; i++) {
    JsonObject relay = relays.createNestedObject();
    relay["channel"] = i;
    relay["label"] = RELAY_LABELS[i];
    relay["description"] = RELAY_DESCRIPTIONS[i];
    relay["state"] = lastRelayState[i];
    relay["pin"] = relayPins[i];
  }
  
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

void handleNotFound(){
  server.send(404, "text/plain", "Not found");
}

void setupWebServer(){
  server.on("/", handleRoot);
  server.on("/json", handleJson);
  server.on("/relay", HTTP_POST, handleRelayControl);
  server.on("/relay-status", handleRelayStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("🌐 WebServer started on port 80");
  Serial.println("   • /          : Dashboard dengan kontrol relay");
  Serial.println("   • /json      : Data real-time JSON");
  Serial.println("   • /relay     : Kontrol relay (POST)");
  Serial.println("   • /relay-status : Status relay JSON");
}

// ================== SETUP ==================
void setup(){
  Serial.begin(115200);
  delay(300);
  setCpuFrequencyMhz(80);
  Serial.printf("⚙  CPU set ke %d MHz\n", getCpuFrequencyMhz());

  for(int i=0;i<4;i++){
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // OFF
  }
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  analogReadResolution(12);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  // ===== WiFi connect
  bool wifiOK = connectWiFiPriority();
  if(!wifiOK){
    startAPFallback();
    wasConnected = false;
  } else {
    wasConnected = true;
  }

  ensureTime();
  configTime(TZ_OFFSET, DST_OFFSET, "pool.ntp.org","time.google.com");

  // Ambil tarif aktif (kalau gagal, default)
  fetchActiveTariffFromSupabase();

  Serial.println("\n🔎 Inisialisasi sensor lingkungan (Guard)...");
  env.begin();

  Serial.println("\n=== PZEM Monitor (ESP32-S3 Mini) — Detail ===");
  Serial.printf("UART1: RX=%d  TX=%d\n", PZEM_RX_PIN, PZEM_TX_PIN);
  PZEMSerial.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);

  loadMonthBaseline();
  loadCalib();
  loadMeterRefs();

  initRelayDefaultsInSupabase();
  fetchRelayConfigFromSupabase();

  setupWebServer();

  Serial.println("\n======================================================");
  Serial.println(" ESP32-S3 + PZEM + BME280/BH1750 + Relay + Supabase + Tarif + WebServer");
  Serial.println(" Interval kirim: 2 detik");
  Serial.println(" Kontrol Relay: Web Interface real-time");
  Serial.println("======================================================\n");
}

// ================== LOOP ==================
void loop(){
  // handle web requests
  server.handleClient();

  // ===== WiFi Connection Management =====
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  
  if(isConnected){
    // Jika sebelumnya tidak terhubung, dan sekarang terhubung
    if (!wasConnected) {
      Serial.println("✅ WiFi terhubung kembali!");
      stopAPFallback(); // Matikan AP jika sedang aktif
      wasConnected = true;
      rguard.markWifiReconn();
      
      // Sinkronisasi waktu ulang
      ensureTime();
      
      // Refresh tarif dari Supabase
      fetchActiveTariffFromSupabase();
    }
    
    // Reset waktu disconnect
    wifiDisconnectedTime = 0;
    
  } else {
    // Jika sebelumnya terhubung, sekarang terputus
    if (wasConnected) {
      Serial.println("⚠ WiFi terputus!");
      wasConnected = false;
      wifiDisconnectedTime = millis();
    }
    
    // Jika sudah lebih dari 8 detik tidak terhubung, coba reconnect
    if (wifiDisconnectedTime > 0 && (millis() - wifiDisconnectedTime > 8000)) {
      // Coba reconnect setiap 30 detik
      if (millis() - lastWifiCheckMs > WIFI_RETRY_INTERVAL_MS) {
        lastWifiCheckMs = millis();
        Serial.println("🔄 Mencoba reconnect WiFi...");
        
        bool reconnected = connectWiFiPriority();
        if (reconnected) {
          wasConnected = true;
          Serial.println("✅ Reconnect berhasil!");
          
          // Matikan AP jika sedang aktif
          stopAPFallback();
          
          // Sinkronisasi waktu ulang
          ensureTime();
          
          // Refresh tarif dari Supabase
          fetchActiveTariffFromSupabase();
        } else {
          Serial.println("❌ Reconnect gagal, tetap di mode AP.");
          
          // Pastikan AP aktif
          startAPFallback();
        }
      }
    }
  }

  // ===== Supabase sync relay & commands (hanya jika WiFi ok) =====
  if (isConnected) {
    fetchRelayConfigFromSupabase();
    pollDeviceCommandsSupabase();
  }

  // ===== Scheduler tiap menit =====
  unsigned long nowMs = millis();
  if(nowMs - lastScheduleTick >= TICK_MS){
    lastScheduleTick = nowMs;
    String hhmm = now_HHMM();
    if(hhmm.length() && hhmm != lastMinuteChecked){
      lastMinuteChecked = hhmm;
      int nowMin = nowMinutes();
      int dIdx   = dayIndex();

      for(int i=0;i<4;i++){
        RelayConfig &cfg = relayCfg[i];
        if (!cfg.valid) continue;

        bool todayEnabled = cfg.day[dIdx];
        if(!todayEnabled) continue;

        bool haveOn  = (cfg.waktuOn.length()  >= 5);
        bool haveOff = (cfg.waktuOff.length() >= 5);

        if (haveOn && haveOff){
          bool shouldBeOn = isInWindow(cfg.waktuOn, cfg.waktuOff, nowMin);
          if(shouldBeOn && !lastRelayState[i]){
            scheduleSetRelay(i, true, "schedule_hold");
          } else if(!shouldBeOn && lastRelayState[i]){
            scheduleSetRelay(i, false, "schedule_hold");
          }
        } else {
          if(haveOn && cfg.waktuOn.substring(0,5)==hhmm && !lastRelayState[i]){
            scheduleSetRelay(i, true, "schedule");
          }
          if(haveOff && cfg.waktuOff.substring(0,5)==hhmm && lastRelayState[i]){
            scheduleSetRelay(i, false, "schedule");
          }
        }
        delay(10);
      }
    }
  }

  // ======= PZEM + energi =======
  {
    PZEMRead r = robustReadPZEM5(80);
    if (r.ok){
      float V  = r.v  * calib.v_gain + calib.v_off;
      float I  = r.i  * calib.i_gain + calib.i_off;
      float Pm = r.p  * calib.p_gain + calib.p_off;
      float E  = r.e;
      float F  = r.f;
      float PF = constrain(r.pf, 0.0f, 1.0f);

      float Pcalc = V * I * PF;
      float mismatch = (Pm>1.0f) ? fabsf(Pm - Pcalc)/Pm : 0.0f;
      mismatch = constrain(mismatch, 0.0f, 1.0f);
      float w_sensor = 1.0f - constrain((mismatch - 0.05f)/0.30f, 0.0f, 1.0f);
      float P = w_sensor*Pm + (1.0f - w_sensor)*Pcalc;

      float S = V * I;
      float Q = NAN;
      if (PF > 0 && PF <= 1.0f){
        float S2=S*S, P2=P*P;
        Q = (S2>P2)? sqrtf(S2-P2):0.0f;
      }

      filt.v=ema(filt.v,V);
      filt.i=ema(filt.i,I);
      filt.p=ema(filt.p,P);
      filt.e=ema(filt.e,E);
      filt.f=ema(filt.f,F);
      filt.pf=ema(filt.pf,PF);
      filt.S=ema(filt.S,S);
      filt.Q=ema(filt.Q,Q);

      autoZeroCurrentIfIdle(filt.i);

      time_t nowt=time(nullptr); struct tm tm_info;
      localtime_r(&nowt,&tm_info);

      auto apply_delta = [&](float delta){
        if (delta>0 && delta < 0.50f){
          float delta_corr = delta * calib.kwh_gain;
          today_kwh += delta_corr;
          month_kwh += delta_corr;
          lastE_kwh = E;
        }
      };

      if (timeReady()){
        int y=tm_info.tm_year+1900;
        int m=tm_info.tm_mon+1;
        int d=tm_info.tm_mday;
        if (lastDay==-1){
          lastYear=y; lastMonth=m; lastDay=d; lastE_kwh=E;
        }
        if (m!=lastMonth || y!=lastYear){
          saveMonthBaseline(month_kwh, lastYear, lastMonth);
          month_kwh=0.0f;
          lastMonth=m; lastYear=y; lastE_kwh=E;
        }
        if (d!=lastDay){
          today_kwh=0.0f;
          lastDay=d; lastE_kwh=E;
        }
        float delta = E - lastE_kwh;
        if (delta < 0){
          lastE_kwh = E;
        } else {
          apply_delta(delta);
        }
      } else {
        if (isnan(lastE_kwh)) lastE_kwh=E;
        float delta = E - lastE_kwh;
        if (delta >= 0) apply_delta(delta);
        else lastE_kwh=E;
      }

      if (millis()-lastPrint > PRINT_EVERY_MS){
        lastPrint = millis();
        printDetailedReadout(filt.v,filt.i,filt.p,E,filt.f,filt.pf,filt.S,filt.Q, tsWIB().c_str());
        Serial.printf("[Ringkasan Energi] Harian: %0.3f kWh | Bulanan: %0.3f kWh | gain(kWh)=%.4f\n",
                      today_kwh, month_kwh, calib.kwh_gain);
      }
    }
  }

  // ======= Sensor Lingkungan =======
  float suhu=NAN, kelembapan=NAN, tekanan=NAN, altitude=NAN, lightLevel=NAN;
  env.read(suhu, kelembapan, tekanan, altitude, lightLevel);

  // ======= Kirim ke Supabase tiap 2 detik (hanya jika WiFi ok) =======
  unsigned long nowMs2 = millis();
  if (nowMs2 - lastSendMs >= SEND_INTERVAL_MS){
    lastSendMs = nowMs2;

    // refresh tarif periodik ringan (opsional) hanya jika WiFi ok
    if (isConnected) {
      static unsigned long lastTariffRefresh = 0;
      if (millis()-lastTariffRefresh > 60000){
        lastTariffRefresh = millis();
        fetchActiveTariffFromSupabase();
      }

      String tsISO = isoTimeWIB();

      bool okLog = sendToSupabaseLog(tsISO, suhu, kelembapan, tekanan, altitude, lightLevel);

      if (okLog){
        digitalWrite(LED_PIN, HIGH);
        delay(200); // LED nyala 200ms
        digitalWrite(LED_PIN, LOW);
      }
    }
  }

  // Small delay untuk memberikan waktu ke sistem
  delay(5);
}
