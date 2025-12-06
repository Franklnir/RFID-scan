#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
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

/* ================== KONFIGURASI HARDWARE (ESP32-C3 + RC522) ================== */
#define SPI_SCK        4   // SCK RC522
#define SPI_MISO       5   // MISO RC522
#define SPI_MOSI       6   // MOSI RC522
#define RFID_SS_PIN   10   // SDA / SS RC522
#define RFID_RST_PIN   9   // RST RC522

// LED & Buzzer
#define LED_PIN      LED_BUILTIN  // LED bawaan (umumnya aktif HIGH di ESP32)
#define BUZZER_PIN   3            // pin buzzer

// Device ID untuk Supabase
const char* DEVICE_ID = "ESP32C3_GERBANG_UTAMA";

/* ================== OBJEK RFID ================== */
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

/* ================== VARIABEL SISTEM ================== */
uint8_t lastUid[10];
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

/* ================== DAFTAR FUNGSI SUARA ================== */
void sfxStartup();
void sfxCardDetected();
void sfxSuccess();
void sfxWarning();
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
String formatTimeIndo(String rawTime);

void sendScanAuto(const String &uidHex);
void sendScanManual(const String &uidHex);
void sendWhatsAppNotif(const String &noWa, const String &nama, const String &kelas, const String &mapel,
                       const String &status, const String &waktuAbsen, const String &deviceId, bool isWarning);

/* ================== HELPER BUZZER (FULL POWER) ================== */
// ESP32 LED built-in umumnya aktif HIGH.
// Kalau board kamu kebalik, tinggal swap HIGH/LOW di 2 baris LED ini.
void beepRaw(int durationMs) {
  digitalWrite(LED_PIN, HIGH);     // LED ON
  digitalWrite(BUZZER_PIN, HIGH);  // Buzzer ON
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);   // Buzzer OFF
  digitalWrite(LED_PIN, LOW);      // LED OFF
}

void beepPattern(int count, int onMs, int offMs) {
  for (int i = 0; i < count; i++) {
    beepRaw(onMs);
    if (i < count - 1) delay(offMs);
  }
}

/* ================== SETUP ================== */
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println("\n\n=== EDUSMART RFID SYSTEM (ESP32-C3 + RC522) ===");
  sfxStartup();

  wifiConnected = connectWiFi();

  // Init SPI & RC522
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, RFID_SS_PIN);
  rfid.PCD_Init();
  delay(50);

  // Test reader
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (v == 0x00 || v == 0xFF) {
    Serial.println("❌ ERROR: RC522 tidak terdeteksi. Cek wiring/power!");
    while (1) {
      sfxError();
      delay(2000);
    }
  }

  checkManualModeFromSupabase();

  Serial.println("✨ SISTEM SIAP ✨");
  beepPattern(2, 150, 150);
}

/* ================== LOOP ================== */
void loop() {
  checkWiFiConnection();
  checkManualModeFromSupabase();

  if (hasLastUid && (millis() - lastScanTime > SCAN_COOLDOWN)) {
    hasLastUid = false;
    lastUidLength = 0;
  }

  // Cek kartu baru
  if (!rfid.PICC_IsNewCardPresent()) {
    delay(5);
    return;
  }
  if (!rfid.PICC_ReadCardSerial()) {
    delay(5);
    return;
  }

  uint8_t uidLength = rfid.uid.size;
  if (uidLength == 0 || uidLength > sizeof(lastUid)) {
    sfxError();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  uint8_t uid[10];
  memcpy(uid, rfid.uid.uidByte, uidLength);

  if (isSameUid(uid, uidLength) && (millis() - lastScanTime < SCAN_COOLDOWN)) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

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

  // stop reader session
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(5);
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
    Serial.println(F("Connect Fail"));
    sfxError();
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  StaticJsonDocument<200> pl;
  pl["p_card_uid"]  = uidHex;
  pl["p_device_id"] = DEVICE_ID;

  String body;
  serializeJson(pl, body);

  int httpCode = http.POST(body);
  String resp = http.getString();

  http.end();
  client.stop();
  delay(20);

  if (httpCode >= 200 && httpCode < 300) {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, resp);
    if (err) {
      Serial.println(F("❌ JSON Error"));
      sfxError();
      return;
    }

    bool success = doc["success"] | false;

    if (success) {
      String nama      = doc["nama"].as<String>();
      String kelas     = doc["kelas"].as<String>();
      String mapel     = doc["mapel"].as<String>();
      String status    = doc["status"].as<String>();
      String hpWali    = doc["no_hp_wali"].as<String>();
      String device    = doc["device_id"].as<String>();
      String waktuRaw  = doc["waktu_absen"].as<String>();

      String waktuIndo = formatTimeIndo(waktuRaw);

      Serial.println(F("✅ BERHASIL"));
      Serial.printf("👤 %s | 🏫 %s | 📚 %s | 📌 %s\n",
                    nama.c_str(), kelas.c_str(), mapel.c_str(), status.c_str());

      String targetWa = normalizePhone62(hpWali);
      bool isDataMissing = false;

      if (targetWa.length() < 10) {
        Serial.println(F("⚠ NO HP KOSONG -> Kirim ke ADMIN DEFAULT"));
        targetWa = normalizePhone62(DEFAULT_WA_TUJUAN);
        isDataMissing = true;
      }

      sfxSuccess();
      sendWhatsAppNotif(targetWa, nama, kelas, mapel, status, waktuIndo, device, isDataMissing);

    } else {
      String reason = doc["reason"].as<String>();
      Serial.printf("⛔ DITOLAK: %s\n", reason.c_str());

      if (reason == "rfid_not_registered") {
        sfxAccessDenied();
      }
      else if (reason == "no_schedule_now" || reason == "holiday") {
        sfxNoSchedule();
      }
      else {
        sfxError();
      }
    }

  } else {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
    sfxError();
  }
}

/* ================== MODE MANUAL ================== */
void sendScanManual(const String &uidHex) {
  if (WiFi.status() != WL_CONNECTED) {
    sfxError();
    return;
  }

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);

  HTTPClient h;

  if (h.begin(c, SUPABASE_MANUAL_URL)) {
    h.addHeader("Content-Type", "application/json");
    h.addHeader("apikey", SUPABASE_KEY);
    h.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

    StaticJsonDocument<200> d;
    d["card_uid"]  = uidHex;
    d["device_id"] = DEVICE_ID;
    d["status"]    = "raw";

    String p;
    serializeJson(d, p);

    int code = h.POST(p);

    if (code >= 200 && code < 300) {
      Serial.println("✅ MANUAL OK");
      sfxSuccess();
    } else {
      Serial.printf("❌ MANUAL ERR: %d\n", code);
      sfxError();
    }
    h.end();
  }
}

/* ================== WHATSAPP NOTIF ================== */
void sendWhatsAppNotif(const String &noWa, const String &nama, const String &kelas, const String &mapel,
                       const String &status, const String &waktu, const String &dev, bool isWarning) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);


  HTTPClient http;
  if (!http.begin(client, MPWA_API_URL)) return;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32C3-Edusmart");

  String message;

  if (isWarning) {
    message  = "⚠ *PERINGATAN SISTEM*\n";
    message += "_Data kontak Wali Murid belum lengkap._\n\n";
  } else {
    message  = "Yth. Orang Tua/Wali Murid,\n\n";
    message += "Berikut kami sampaikan laporan absensi siswa:\n\n";
  }

  message += "📋 *DATA ABSENSI*\n";
  message += "━━━━━━━━━━━━━━━━━━\n";
  message += "👤 Nama    : *" + nama + "*\n";
  message += "🏫 Kelas   : " + kelas + "\n";
  message += "✅ Status  : *" + status + "*\n";

  if (mapel != "" && mapel != "null") {
    message += "📖 Mapel   : " + mapel + "\n";
  }

  message += "🕒 Waktu   : " + waktu + "\n";
  message += "━━━━━━━━━━━━━━━━━━\n";
  message += "🧭 Device  : " + dev + "\n\n";
  message += "Pesan ini dikirim otomatis oleh sistem sekolah.\n";
  message += "Hormat Kami,\n*" + String(MPWA_FOOTER) + "*";

  StaticJsonDocument<1024> doc;
  doc["api_key"] = MPWA_API_KEY;
  doc["sender"]  = MPWA_SENDER;
  doc["number"]  = noWa;
  doc["message"] = message;
  doc["footer"]  = MPWA_FOOTER;
  doc["full"]    = 0;

  String body;
  serializeJson(doc, body);

  http.POST(body);
  http.end();
  client.stop();

  Serial.println(F("📩 WA Terkirim"));
}

/* ================== SUARA "TIT TIT" ================== */
void sfxStartup() {
  beepPattern(3, 120, 80);
}

void sfxCardDetected() {
  beepPattern(1, 80, 0);
}

void sfxSuccess() {
  beepPattern(1, 220, 0);
}

void sfxWarning() {
  beepPattern(4, 100, 80);
}

void sfxNoSchedule() {
  beepPattern(3, 200, 120);
}

void sfxAccessDenied() {
  beepPattern(3, 180, 200);
}

void sfxError() {
  beepPattern(2, 300, 250);
}

/* ================== UTILITY & FORMATTING ================== */
String formatTimeIndo(String rawTime) {
  if (rawTime.length() < 16) return rawTime;

  String tahun   = rawTime.substring(0, 4);
  String bulan   = rawTime.substring(5, 7);
  String tanggal = rawTime.substring(8, 10);
  String jam     = rawTime.substring(11, 16);

  return tanggal + "-" + bulan + "-" + tahun + ", Pukul " + jam + " WIB";
}

void checkManualModeFromSupabase() {
  if (millis() - lastModeCheck < MODE_CHECK_INTERVAL) return;
  lastModeCheck = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(12000);

  HTTPClient h;

  if (h.begin(c, SUPABASE_MODE_URL)) {
    h.addHeader("apikey", SUPABASE_KEY);
    h.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

    int code = h.GET();
    if (code == 200) {
      StaticJsonDocument<192> d;
      DeserializationError err = deserializeJson(d, h.getString());

      if (!err && d.is<JsonArray>() && d.size() > 0) {
        bool n = d[0]["scan_manual_enabled"];

        if (n != manualModeEnabled) {
          manualModeEnabled = n;
          beepPattern(2, 150, 150);
          Serial.printf("🔁 Mode manual: %s\n", manualModeEnabled ? "ON" : "OFF");
        }
      }
    } else {
      Serial.printf("⚠ Gagal cek mode (%d)\n", code);
    }
    h.end();
  }
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int r = 0;
  Serial.print("📶 Menghubungkan WiFi");

  while (WiFi.status() != WL_CONNECTED && r < 20) {
    delay(400);
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    Serial.print(".");
    r++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("✅ WiFi OK: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_PIN, LOW);
    return true;
  } else {
    Serial.println("❌ WiFi Gagal");
    digitalWrite(LED_PIN, LOW);
    return false;
  }
}

void checkWiFiConnection() {
  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("🔄 Reconnect WiFi...");
      connectWiFi();
    }
  }
}

String normalizePhone62(const String &raw) {
  String s = raw;
  s.trim();
  s.replace("-", "");
  s.replace(" ", "");
  s.replace("+", "");
  if (s.startsWith("0")) s = "62" + s.substring(1);
  return s;
}

String uidToHexString(uint8_t *uid, uint8_t len) {
  String s;
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

bool isSameUid(uint8_t *uid, uint8_t uidLength) {
  if (!hasLastUid || uidLength != lastUidLength) return false;
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] != lastUid[i]) return false;
  }
  return true;
}
