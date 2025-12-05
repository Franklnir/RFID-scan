#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>

/* ================== KONFIGURASI JARINGAN ================== */
const char* WIFI_SSID = "GEORGIA";
const char* WIFI_PASS = "Georgia12345";

/* ================== KONFIGURASI SUPABASE ================== */
const char* SUPABASE_RPC_URL    = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/rpc/absensi_rfid_auto";
const char* SUPABASE_MANUAL_URL = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/rfid_scans";
const char* SUPABASE_MODE_URL   = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/settings?select=scan_manual_enabled&limit=1";
const char* SUPABASE_KEY        = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InpueG1rYXN0emJwemZ1cnp0dnd0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjIwODYwNTUsImV4cCI6MjA3NzY2MjA1NX0.-1OhFVh6XIkQm2tabzzF_xEOLgaOXhdTrYuKP7rQssQ";

/* ================== KONFIGURASI WHATSAPP ================== */
const char* MPWA_API_URL      = "https://gateway.asr-desain.my.id/send-message";
const char* MPWA_API_KEY      = "AezFYI3JBhi0Aq96JRoUyMpMdKEs9Oaw";
const char* MPWA_SENDER       = "6289531832365";
const char* MPWA_FOOTER       = "Sistem Informasi Akademik - Edusmart";
const char* DEFAULT_WA_TUJUAN = "6281285493442"; 

/* ================== KONFIGURASI HARDWARE ================== */
#define PN532_SCK   D5
#define PN532_MISO  D6
#define PN532_MOSI  D7
#define PN532_SS    D0

#define LED_PIN      LED_BUILTIN 
#define BUZZER_PIN   D2          

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

/* ================== VARIABEL SISTEM ================== */
uint8_t lastUid[7];
uint8_t lastUidLength = 0;
bool hasLastUid = false;
unsigned long lastScanTime = 0;
const unsigned long SCAN_COOLDOWN = 4000;

bool wifiConnected = false;
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 15000;

bool manualModeEnabled = false;
unsigned long lastModeCheck = 0;
const unsigned long MODE_CHECK_INTERVAL = 15000;

/* ================== DAFTAR FUNGSI SUARA (SFX HIGH PITCH) ================== */
void sfxStartup();       
void sfxCardDetected();  
void sfxSuccess();       // Nada Tinggi (Melengking Bahagia)
void sfxWarning();       // Nada Alarm Cepat
void sfxNoSchedule();    
void sfxAccessDenied();  
void sfxError();         

/* ================== FUNGSI UTAMA ================== */
bool connectWiFi();
void checkWiFiConnection();
void checkManualModeFromSupabase();
String uidToHexString(uint8_t *uid, uint8_t len);
bool isSameUid(uint8_t *uid, uint8_t uidLength);
String normalizePhone62(const String &raw);
String formatTimeIndo(String rawTime); // Fungsi baru untuk format waktu

void sendScanAuto(const String &uidHex);
void sendScanManual(const String &uidHex);
void sendWhatsAppNotif(const String &noWa, const String &nama, const String &kelas, const String &mapel, const String &status, const String &waktuAbsen, const String &deviceId, bool isWarning);

/* ================== SETUP ================== */
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  noTone(BUZZER_PIN);

  Serial.println("\n\n=== EDUSMART RFID SYSTEM ===");
  sfxStartup();

  wifiConnected = connectWiFi();

  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println(F("❌ ERROR: PN532 Fail!"));
    while (1) { sfxError(); delay(2000); }
  }
  nfc.SAMConfig();

  checkManualModeFromSupabase();
  Serial.println(F("✨ SISTEM SIAP ✨"));
  
  // Bunyi beep keras tanda siap
  tone(BUZZER_PIN, 3000, 100); delay(100); noTone(BUZZER_PIN);
}

/* ================== LOOP ================== */
void loop() {
  checkWiFiConnection();
  checkManualModeFromSupabase();

  if (hasLastUid && (millis() - lastScanTime > SCAN_COOLDOWN)) {
    hasLastUid = false;
    lastUidLength = 0;
  }

  uint8_t uid[7];
  uint8_t uidLength;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
    if (isSameUid(uid, uidLength) && (millis() - lastScanTime < SCAN_COOLDOWN)) return; 

    // 1. Feedback Awal (Kartu Terbaca - Suara Tajam)
    sfxCardDetected();
    
    lastScanTime = millis();
    memcpy(lastUid, uid, uidLength);
    lastUidLength = uidLength;
    hasLastUid = true;

    String uidHex = uidToHexString(uid, uidLength);
    
    Serial.println(F("\n╔══════════════════════════════╗"));
    Serial.printf("║ UID : %s\n", uidHex.c_str());
    Serial.println(F("╚══════════════════════════════╝"));
    
    if (manualModeEnabled) sendScanManual(uidHex);
    else sendScanAuto(uidHex);
  }
  yield();
}

/* ================== LOGIC UTAMA (AUTO) ================== */
void sendScanAuto(const String &uidHex) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("⚠ WiFi Error"));
    sfxError();
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);
  HTTPClient http;

  Serial.print(F("🔄 Cek Jadwal... "));
  if (!http.begin(client, SUPABASE_RPC_URL)) {
    Serial.println(F("Connect Fail")); sfxError(); return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  StaticJsonDocument<200> pl;
  pl["p_card_uid"] = uidHex;
  pl["p_device_id"] = "WEMOS_D1_GERBANG_UTAMA";
  String body; serializeJson(pl, body);

  int httpCode = http.POST(body);
  String resp = http.getString();
  http.end(); client.stop();
  delay(50); yield();

  if (httpCode >= 200 && httpCode < 300) {
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, resp);
    bool success = doc["success"] | false;

    if (success) {
      String nama = doc["nama"].as<String>();
      String kelas = doc["kelas"].as<String>();
      String mapel = doc["mapel"].as<String>();
      String status = doc["status"].as<String>();
      String hpWali = doc["no_hp_wali"].as<String>();
      String device = doc["device_id"].as<String>();
      String waktuRaw = doc["waktu_absen"].as<String>();
      
      // Format waktu agar lebih cantik di WA
      String waktuIndo = formatTimeIndo(waktuRaw);

      Serial.println(F("✅ BERHASIL"));
      Serial.printf("👤 %s | 🏫 %s\n", nama.c_str(), kelas.c_str());

      String targetWa = normalizePhone62(hpWali);
      bool isDataMissing = false;

      if (targetWa.length() < 10) { 
        Serial.println(F("⚠ NO HP KOSONG -> Kirim Admin"));
        targetWa = normalizePhone62(DEFAULT_WA_TUJUAN);
        isDataMissing = true;
        sfxWarning(); 
      } else {
        sfxSuccess(); 
      }

      sendWhatsAppNotif(targetWa, nama, kelas, mapel, status, waktuIndo, device, isDataMissing);

    } else {
      String reason = doc["reason"].as<String>();
      Serial.printf("⛔ DITOLAK: %s\n", reason.c_str());

      if (reason == "rfid_not_registered") sfxAccessDenied(); 
      else if (reason == "no_schedule_now" || reason == "holiday") sfxNoSchedule();
      else sfxError();
    }
  } else {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
    sfxError();
  }
}

void sendWhatsAppNotif(const String &noWa, const String &nama, const String &kelas, const String &mapel, const String &status, const String &waktu, const String &dev, bool isWarning) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);
  client.setBufferSizes(512, 512);
  HTTPClient http;

  if (!http.begin(client, MPWA_API_URL)) return;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP8266-Edusmart");

  String message;
  
  if (isWarning) {
    message = "⚠ *PERINGATAN SISTEM*\n";
    message += "_Data kontak Wali Murid belum lengkap._\n\n";
  } else {
    // Salam Formal
    message = "Yth. Orang Tua/Wali Murid,\n\n";
    message += "Berikut kami sampaikan laporan absensi siswa:\n\n";
  }
  
  // Body Pesan Formal
  message += "📋 *DATA ABSENSI*\n";
  message += "━━━━━━━━━━━━━━━━━━\n";
  message += "👤 Nama    : *" + nama + "*\n";
  message += "🏫 Kelas   : " + kelas + "\n";
  message += "✅ Status  : *" + status + "*\n";
  
  if (mapel != "" && mapel != "null") {
    message += "📖 Mapel   : " + mapel + "\n";
  }
  
  message += "🕒 Waktu   : " + waktu + "\n";
  message += "━━━━━━━━━━━━━━━━━━\n\n";
  message += "Pesan ini dikirim otomatis oleh sistem sekolah.\n";
  message += "Hormat Kami,\n*" + String(MPWA_FOOTER) + "*";

  StaticJsonDocument<1024> doc;
  doc["api_key"] = MPWA_API_KEY;
  doc["sender"] = MPWA_SENDER;
  doc["number"] = noWa;
  doc["message"] = message;
  doc["footer"] = MPWA_FOOTER;
  doc["full"] = 0;

  String body; serializeJson(doc, body);
  http.POST(body);
  http.end(); client.stop();
  Serial.println(F("📩 WA Terkirim"));
}

void sendScanManual(const String &uidHex) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure c; c.setInsecure(); HTTPClient h;
  if (h.begin(c, SUPABASE_MANUAL_URL)) {
    h.addHeader("Content-Type", "application/json");
    h.addHeader("apikey", SUPABASE_KEY);
    h.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
    StaticJsonDocument<200> d; d["card_uid"] = uidHex; d["device_id"] = "WEMOS"; d["status"] = "raw";
    String p; serializeJson(d, p);
    if(h.POST(p) >= 200) { Serial.println("✅ OK"); sfxSuccess(); } else sfxError();
    h.end();
  }
}

/* ================== SUARA LEBIH KENCANG & TAJAM ================== */
// Frekuensi dinaikkan ke 2000Hz - 3500Hz agar melengking

void sfxStartup() {
  // Naik Tajam
  tone(BUZZER_PIN, 2000, 100); delay(100);
  tone(BUZZER_PIN, 2500, 100); delay(100);
  tone(BUZZER_PIN, 3000, 200); delay(200);
  noTone(BUZZER_PIN);
}

void sfxCardDetected() {
  // Pip sangat singkat dan tinggi
  tone(BUZZER_PIN, 3500, 50); delay(60); noTone(BUZZER_PIN);
}

void sfxSuccess() {
  // Ding-Dong Kencang (Melengking)
  tone(BUZZER_PIN, 2500, 100); delay(120); 
  tone(BUZZER_PIN, 3200, 300); delay(300); 
  noTone(BUZZER_PIN);
}

void sfxWarning() {
  // Alarm Cepat
  tone(BUZZER_PIN, 2500, 80); delay(100);
  tone(BUZZER_PIN, 2500, 80); delay(100);
  tone(BUZZER_PIN, 2500, 80); delay(100);
  noTone(BUZZER_PIN);
}

void sfxNoSchedule() {
  // Nada Datar (Menengah)
  tone(BUZZER_PIN, 1000, 200); delay(250);
  tone(BUZZER_PIN, 1000, 200); delay(250);
  noTone(BUZZER_PIN);
}

void sfxAccessDenied() {
  // Nada Jatuh (Uh-oh)
  tone(BUZZER_PIN, 2000, 100); delay(100);
  tone(BUZZER_PIN, 1500, 100); delay(100);
  tone(BUZZER_PIN, 500, 400); delay(400);
  noTone(BUZZER_PIN);
}

void sfxError() {
  // Kasar dan Rendah (Buzzer getar)
  for(int i=0; i<3; i++) {
    tone(BUZZER_PIN, 150, 200); delay(250);
  }
  noTone(BUZZER_PIN);
}

/* ================== UTILITY & FORMATTING ================== */

// Fungsi mengubah "2025-12-05 07:00:00" -> "05-12-2025, Pukul 07:00 WIB"
String formatTimeIndo(String rawTime) {
  // Raw biasanya: YYYY-MM-DDTHH:MM:SS atau YYYY-MM-DD HH:MM:SS
  // Kita ambil bagian-bagiannya
  if (rawTime.length() < 16) return rawTime; // Kalau format aneh, kembalikan asli

  String tahun = rawTime.substring(0, 4);
  String bulan = rawTime.substring(5, 7);
  String tanggal = rawTime.substring(8, 10);
  String jam = rawTime.substring(11, 16); // Ambil HH:MM saja

  return tanggal + "-" + bulan + "-" + tahun + ", Pukul " + jam + " WIB";
}

void checkManualModeFromSupabase() {
  if (millis() - lastModeCheck < MODE_CHECK_INTERVAL) return;
  lastModeCheck = millis();
  WiFiClientSecure c; c.setInsecure(); HTTPClient h;
  if (h.begin(c, SUPABASE_MODE_URL)) {
    h.addHeader("apikey", SUPABASE_KEY); h.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
    if (h.GET() == 200) {
      StaticJsonDocument<192> d; deserializeJson(d, h.getString());
      if (d.is<JsonArray>() && d.size() > 0) {
        bool n = d[0]["scan_manual_enabled"];
        if (n != manualModeEnabled) {
          manualModeEnabled = n;
          tone(BUZZER_PIN, 3000, 100); delay(100); noTone(BUZZER_PIN);
        }
      }
    } h.end();
  }
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  int r = 0; while (WiFi.status() != WL_CONNECTED && r < 20) {
    delay(400); digitalWrite(LED_PIN, !digitalRead(LED_PIN)); r++;
  }
  return WiFi.status() == WL_CONNECTED;
}

void checkWiFiConnection() {
  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
  }
}

String normalizePhone62(const String &raw) {
  String s = raw; s.trim(); s.replace("-", ""); s.replace(" ", ""); s.replace("+", "");
  if (s.startsWith("0")) s = "62" + s.substring(1);
  return s;
}

String uidToHexString(uint8_t *uid, uint8_t len) {
  String s; for (uint8_t i = 0; i < len; i++) { if (uid[i] < 0x10) s += "0"; s += String(uid[i], HEX); }
  s.toUpperCase(); return s;
}

bool isSameUid(uint8_t *uid, uint8_t uidLength) {
  if (!hasLastUid || uidLength != lastUidLength) return false;
  for (uint8_t i = 0; i < uidLength; i++) if (uid[i] != lastUid[i]) return false;
  return true;
}
