#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>

/* ================== KONFIGURASI JARINGAN ================== */
const char* WIFI_SSID = "GEORGIA";
const char* WIFI_PASS = "Georgia12345";

/* ================== KONFIGURASI DEVICE ================== */
const char* DEVICE_ID = "WEMOS_D1_GERBANG_UTAMA";

/* ================== KONFIGURASI SUPABASE ================== */
const char* SUPABASE_RPC_URL    = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/rpc/absensi_rfid_auto";
const char* SUPABASE_MANUAL_URL = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/rfid_scans";
const char* SUPABASE_MODE_URL   = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/settings?select=scan_manual_enabled&limit=1";
const char* SUPABASE_KEY        = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InpueG1rYXN0emJwemZ1cnp0dnd0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjIwODYwNTUsImV4cCI6MjA3NzY2MjA1NX0.-1OhFVh6XIkQm2tabzzF_xEOLgaOXhdTrYuKP7rQssQ";

/* ================== KONFIGURASI WHATSAPP ================== */
const char* MPWA_API_URL      = "https://gateway.asr-desain.my.id/send-message";
const char* MPWA_API_KEY      = "AezFYI3JBhi0Aq96JRoUyMpMdKEs9Oaw";
const char* MPWA_SENDER       = "6289531832365";
const char* MPWA_FOOTER       = "Edusmart System";
const char* DEFAULT_WA_TUJUAN = "6281285493442"; // Nomor Admin/Cadangan

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

/* ================== DAFTAR FUNGSI SUARA (SFX) ================== */
void sfxStartup();       // Saat nyala
void sfxCardDetected();  // Saat kartu nempel (Pip)
void sfxSuccess();       // Absen Berhasil (Ding-Dong)
void sfxWarning();       // Berhasil tapi No HP Kosong (Tet-Tet)
void sfxNoSchedule();    // Tidak ada jadwal (Bip-Bop Flat)
void sfxAccessDenied();  // Kartu tidak terdaftar (Nada Turun)
void sfxError();         // Error Server/WiFi (Buzz Kasar)

/* ================== FUNGSI UTAMA ================== */
bool connectWiFi();
void checkWiFiConnection();
void checkManualModeFromSupabase();
String uidToHexString(uint8_t *uid, uint8_t len);
bool isSameUid(uint8_t *uid, uint8_t uidLength);
String normalizePhone62(const String &raw);
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
    while (1) { 
      sfxError(); 
      delay(2000); 
    }
  }
  nfc.SAMConfig();

  checkManualModeFromSupabase();
  Serial.println(F("✨ SISTEM SIAP ✨"));
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

    // 1. Feedback Awal (Kartu Terbaca)
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
    Serial.println(F("Connect Fail")); 
    sfxError(); 
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  StaticJsonDocument<200> pl;
  pl["p_card_uid"] = uidHex;
  pl["p_device_id"] = DEVICE_ID;
  String body; 
  serializeJson(pl, body);

  int httpCode = http.POST(body);
  String resp = http.getString();
  http.end(); 
  client.stop();
  delay(50); 
  yield();

  if (httpCode >= 200 && httpCode < 300) {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, resp);
    if (err) {
      Serial.println(F("❌ JSON Parse Error"));
      sfxError();
      return;
    }

    bool success = doc["success"] | false;

    if (success) {
      String nama   = doc["nama"].as<String>();
      String kelas  = doc["kelas"].as<String>();
      String mapel  = doc["mapel"].as<String>();
      String status = doc["status"].as<String>();
      String hpWali = doc["no_hp_wali"].as<String>();
      String device = doc["device_id"].as<String>();
      String waktu  = doc["waktu_absen"].as<String>();

      Serial.println(F("✅ BERHASIL"));
      Serial.printf("👤 %s | 🏫 %s\n", nama.c_str(), kelas.c_str());

      // --- LOGIKA NO HP KOSONG ---
      String targetWa = normalizePhone62(hpWali);
      bool isDataMissing = false;

      if (targetWa.length() < 10) { 
        // Kasus: No HP Tidak Ada / Tidak Valid
        Serial.println(F("⚠ NO HP WALI KOSONG/INVALID -> Kirim ke Admin"));
        targetWa = normalizePhone62(DEFAULT_WA_TUJUAN);
        isDataMissing = true;
        sfxWarning(); // Bunyi Warning (Tet-Tet)
      } else {
        // Kasus: Normal
        sfxSuccess(); // Bunyi Sukses (Ding-Dong)
      }

      sendWhatsAppNotif(targetWa, nama, kelas, mapel, status, waktu, device, isDataMissing);

    } else {
      // --- LOGIKA GAGAL (Reason handling) ---
      String reason = doc["reason"].as<String>();
      String msg    = doc["message"].as<String>();
      Serial.printf("⛔ DITOLAK: %s\n", reason.c_str());

      if (reason == "rfid_not_registered") {
        // Kasus: Kartu Tidak Terdaftar
        Serial.println(F(">> Kartu Ilegal / Belum Daftar"));
        sfxAccessDenied(); 
      } 
      else if (reason == "no_schedule_now" || reason == "holiday") {
        // Kasus: Tidak Ada Jadwal / Libur / Di luar jam
        Serial.println(F(">> Tidak ada jadwal pelajaran saat ini"));
        sfxNoSchedule();
      } 
      else {
        // Error lain
        sfxError();
      }
    }
  } else {
    Serial.printf("❌ HTTP Error: %d\n", httpCode);
    sfxError();
  }
}

/* ================== WHATSAPP NOTIF (RAPIH & PROFESIONAL) ================== */
void sendWhatsAppNotif(
  const String &noWa,
  const String &nama,
  const String &kelas,
  const String &mapel,
  const String &status,
  const String &waktu,
  const String &dev,
  bool isWarning
) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);
  client.setBufferSizes(512, 512);
  HTTPClient http;

  if (!http.begin(client, MPWA_API_URL)) {
    Serial.println(F("❌ Gagal init HTTP untuk MPWA"));
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP8266-Edusmart");

  String message;
  String mapelLine = "";

  if (mapel != "" && mapel != "null") {
    mapelLine = "📖 Mata Pelajaran : " + mapel + "\n";
  }

  if (isWarning) {
    // === MODE WARNING: Kirim ke Admin (data HP wali kosong/invalid) ===
    message  = "⚠ *PERINGATAN DATA SISWA*\n\n";
    message += "Nomor HP orang tua/wali *belum diisi* atau *tidak valid* pada sistem.\n\n";
    message += "Detail siswa:\n";
    message += "👤 Nama   : " + nama + "\n";
    message += "🏫 Kelas  : " + kelas + "\n";
    if (mapelLine.length() > 0) message += mapelLine;
    message += "✅ Status : " + status + "\n";
    message += "🕒 Waktu  : " + waktu + "\n";
    message += "📍 Perangkat : " + dev + "\n\n";
    message += "Dimohon untuk memperbarui data kontak orang tua/wali pada sistem *Edusmart*.\n";
  } else {
    // === MODE NORMAL: Kirim ke Orang Tua/Wali ===
    message  = "🔔 *LAPORAN ABSENSI SISWA*\n\n";
    message += "Yth. Bapak/Ibu Orang Tua/Wali,\n\n";
    message += "Berikut kami informasikan kehadiran putra/putri Bapak/Ibu:\n\n";
    message += "👤 Nama   : " + nama + "\n";
    message += "🏫 Kelas  : " + kelas + "\n";
    if (mapelLine.length() > 0) message += mapelLine;
    message += "✅ Status : " + status + "\n";
    message += "🕒 Waktu  : " + waktu + "\n";
    message += "📍 Perangkat : " + dev + "\n\n";
    message += "Terima kasih atas perhatian Bapak/Ibu.\n";
  }

  StaticJsonDocument<768> doc;
  doc["api_key"] = MPWA_API_KEY;
  doc["sender"]  = MPWA_SENDER;
  doc["number"]  = noWa;
  doc["message"] = message;
  doc["footer"]  = MPWA_FOOTER;
  doc["full"]    = 0;

  String body; 
  serializeJson(doc, body);
  int httpCode = http.POST(body);

  if (httpCode >= 200 && httpCode < 300) {
    Serial.println(F("📩 WA Terkirim"));
  } else {
    Serial.printf("❌ Gagal kirim WA (HTTP %d)\n", httpCode);
  }

  http.end(); 
  client.stop();
}

void sendScanManual(const String &uidHex) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure c; 
  c.setInsecure(); 
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
    if (h.POST(p) >= 200) { 
      Serial.println("✅ OK"); 
      sfxSuccess(); 
    } else {
      sfxError(); 
    }
    h.end();
  }
}

/* ================== PERPUSTAKAAN SFX (SOUND EFFECTS) ================== */
/* Catatan: "Dikencangkan" di sini artinya durasi & pattern
   dibuat lebih panjang & tegas supaya lebih jelas terdengar. */

// 1. STARTUP: Do-Mi-Sol (lebih panjang & jelas)
void sfxStartup() {
  tone(BUZZER_PIN, 523, 200);  // C5
  delay(220);
  tone(BUZZER_PIN, 659, 200);  // E5
  delay(220);
  tone(BUZZER_PIN, 784, 350);  // G5
  delay(380);
  noTone(BUZZER_PIN);
}

// 2. KARTU TERDETEKSI: 2x Pip cepat (lebih terasa)
void sfxCardDetected() {
  for (int i = 0; i < 2; i++) {
    tone(BUZZER_PIN, 2200, 90);
    delay(120);
  }
  noTone(BUZZER_PIN);
}

// 3. SUKSES SEMPURNA: Ding-Dong (lebih panjang)
void sfxSuccess() {
  tone(BUZZER_PIN, 1047, 220); // C6
  delay(250);
  tone(BUZZER_PIN, 1319, 350); // E6
  delay(380);
  noTone(BUZZER_PIN);
}

// 4. WARNING (NO HP KOSONG): Tet-Tet (lebih lama)
void sfxWarning() {
  tone(BUZZER_PIN, 880, 180);
  delay(220);
  tone(BUZZER_PIN, 880, 180);
  delay(220);
  noTone(BUZZER_PIN);
}

// 5. TIDAK ADA JADWAL: Nada Datar (Netral, tapi jelas)
void sfxNoSchedule() {
  tone(BUZZER_PIN, 500, 260); 
  delay(300);
  tone(BUZZER_PIN, 500, 260); 
  delay(300);
  noTone(BUZZER_PIN);
}

// 6. KARTU ILEGAL: Nada Turun (lebih dramatis)
void sfxAccessDenied() {
  tone(BUZZER_PIN, 900, 200); 
  delay(230);
  tone(BUZZER_PIN, 650, 200); 
  delay(230);
  tone(BUZZER_PIN, 450, 450); 
  delay(480);
  noTone(BUZZER_PIN);
}

// 7. ERROR SISTEM/WIFI: Buzz Kasar (lebih panjang & 3x)
void sfxError() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 120, 350); 
    delay(400);
  }
  noTone(BUZZER_PIN);
}

/* ================== UTILITY ================== */
void checkManualModeFromSupabase() {
  if (millis() - lastModeCheck < MODE_CHECK_INTERVAL) return;
  lastModeCheck = millis();

  WiFiClientSecure c; 
  c.setInsecure(); 
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
          // Bunyi pendek tanda mode berubah
          tone(BUZZER_PIN, 1500, 150); 
          delay(180); 
          noTone(BUZZER_PIN);
        }
      }
    }
    h.end();
  }
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA); 
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int r = 0; 
  while (WiFi.status() != WL_CONNECTED && r < 20) {
    delay(400); 
    digitalWrite(LED_PIN, !digitalRead(LED_PIN)); 
    r++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, LOW);
    Serial.print(F("📶 WiFi OK: "));
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println(F("❌ Gagal konek WiFi"));
  return false;
}

void checkWiFiConnection() {
  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("🔁 Reconnect WiFi..."));
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
  for (uint8_t i = 0; i < uidLength; i++) 
    if (uid[i] != lastUid[i]) return false;
  return true;
}
