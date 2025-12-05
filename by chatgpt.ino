#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>   // Pastikan versi 6.x atau 7.x

/* ================== Konfigurasi WiFi ================== */
const char* WIFI_SSID = "GEORGIA";
const char* WIFI_PASS = "Georgia12345";

/* ================== Konfigurasi Device ================== */
const char* DEVICE_ID = "WEMOS_D1_GERBANG_UTAMA";

/* ================== Konfigurasi Supabase ================== */
const char* SUPABASE_RPC_URL    = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/rpc/absensi_rfid_auto";
const char* SUPABASE_MANUAL_URL = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/rfid_scans";
const char* SUPABASE_MODE_URL   = "https://znxmkastzbpzfurztvwt.supabase.co/rest/v1/settings?select=scan_manual_enabled&limit=1";
const char* SUPABASE_KEY        = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InpueG1rYXN0emJwemZ1cnp0dnd0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjIwODYwNTUsImV4cCI6MjA3NzY2MjA1NX0.-1OhFVh6XIkQm2tabzzF_xEOLgaOXhdTrYuKP7rQssQ";

/* ================== Konfigurasi MPWA WhatsApp ================== */
const char* MPWA_API_URL      = "https://gateway.asr-desain.my.id/send-message";
const char* MPWA_API_KEY      = "AezFYI3JBhi0Aq96JRoUyMpMdKEs9Oaw";
const char* MPWA_SENDER       = "6289531832365";
const char* MPWA_FOOTER       = "Edusmart";
const char* DEFAULT_WA_TUJUAN = "6281285493442";

/* ================== Konfigurasi Hardware ================== */
// PN532 Wiring (Software SPI)
#define PN532_SCK   D5
#define PN532_MISO  D6
#define PN532_MOSI  D7
#define PN532_SS    D0

// LED & Buzzer
#define LED_PIN      LED_BUILTIN // Aktif LOW
#define BUZZER_PIN   D2

Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

/* ================== Variabel Global ================== */
uint8_t lastUid[7];
uint8_t lastUidLength = 0;
bool hasLastUid = false;
unsigned long lastScanTime = 0;
const unsigned long SCAN_COOLDOWN = 4000; // 4 detik cooldown

bool wifiConnected = false;
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 15000; // 15 detik

bool manualModeEnabled = false;
unsigned long lastModeCheck = 0;
const unsigned long MODE_CHECK_INTERVAL = 15000;

/* ================== Deklarasi Fungsi ================== */
bool   connectWiFi();
void   checkWiFiConnection();
void   checkManualModeFromSupabase();

bool   isSameUid(uint8_t *uid, uint8_t uidLength);
String uidToHexString(uint8_t *uid, uint8_t len);

void   beepAndBlink(int count);
void   blinkLED(int count, int delayTime);
void   longBeep(int durationMs);

String normalizePhone62(const String &raw);

void sendScanAuto(const String &uidHex);
void sendScanManual(const String &uidHex);
void sendWhatsAppNotif(
  const String &noWa,
  const String &nama,
  const String &kelas,
  const String &mapel,
  const String &status,
  const String &waktuAbsen,
  const String &deviceId,
  const String &cardUid
);

/* ================== SETUP ================== */

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // LED off (aktif LOW)
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println();
  Serial.println("====================================");
  Serial.println("   WEMOS D1 MINI RFID ABSENSI");
  Serial.println("====================================");

  // Test LED + Buzzer
  Serial.println("Test LED dan Buzzer...");
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(300);
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);
  delay(200);

  // Koneksi WiFi
  wifiConnected = connectWiFi();

  // Init PN532
  Serial.println("\n--- INISIALISASI PN532 ---");
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("❌ ERROR: PN532 tidak ditemukan!");
    Serial.println("   Cek wiring:");
    Serial.println("   - VCC  -> 3.3V");
    Serial.println("   - GND  -> GND");
    Serial.println("   - SCK  -> D5");
    Serial.println("   - MISO -> D6");
    Serial.println("   - MOSI -> D7");
    Serial.println("   - SS   -> D0");
    while (1) {
      blinkLED(2, 200);
      delay(800);
      yield();
    }
  }

  Serial.print("✅ PN532 Ready. Firmware: ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print(".");
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  nfc.SAMConfig(); // Active read mode

  // Cek mode manual pertama kali
  checkManualModeFromSupabase();

  Serial.println("\n--- SISTEM SIAP ---");
  Serial.println("Tempelkan kartu RFID/NFC ke reader...");
  Serial.println("====================================");

  // Indikator siap
  beepAndBlink(2);
}

/* ================== LOOP ================== */

void loop() {
  // Cek WiFi dan Mode Manual secara periodik
  checkWiFiConnection();
  checkManualModeFromSupabase();

  // Reset last UID jika cooldown habis
  if (hasLastUid && (millis() - lastScanTime > SCAN_COOLDOWN)) {
    hasLastUid = false;
    lastUidLength = 0;
    Serial.println("[System] Cooldown selesai, siap scan kartu baru...");
  }

  uint8_t uid[7];
  uint8_t uidLength;

  // Timeout 50ms biar loop nggak nge-freeze
  bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50);

  if (found) {
    // Kalau kartu sama & masih dalam cooldown → abaikan
    if (isSameUid(uid, uidLength) && (millis() - lastScanTime < SCAN_COOLDOWN)) {
      // Serial.println("[System] Kartu sama dalam cooldown, di-skip...");
      return;
    }

    lastScanTime = millis();
    memcpy(lastUid, uid, uidLength);
    lastUidLength = uidLength;
    hasLastUid = true;

    String uidHex = uidToHexString(uid, uidLength);

    Serial.println();
    Serial.println("🎊 KARTU TERDETEKSI!");
    Serial.println("=================================");
    Serial.print("UID: ");
    Serial.println(uidHex);
    Serial.print("Panjang: ");
    Serial.print(uidLength);
    Serial.println(" byte");

    if (uidLength == 4) {
      Serial.println("Tipe: MIFARE Classic 1K/4K");
    } else if (uidLength == 7) {
      Serial.println("Tipe: MIFARE DESFire/Ultralight");
    } else {
      Serial.println("Tipe: Kartu NFC lainnya");
    }
    Serial.println("=================================");

    // Feedback lokal
    beepAndBlink(1);

    if (manualModeEnabled) {
      Serial.println("📡 MODE: MANUAL → kirim RAW ke rfid_scans");
      sendScanManual(uidHex);
    } else {
      Serial.println("📡 MODE: OTOMATIS → panggil RPC absensi_rfid_auto");
      sendScanAuto(uidHex);
    }
  } else {
    // Heartbeat tiap 5 detik
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat > 5000) {
      lastHeartbeat = millis();
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("💚 Sistem OK | WiFi RSSI: ");
        Serial.println(WiFi.RSSI());
        digitalWrite(LED_PIN, LOW);
        delay(50);
        digitalWrite(LED_PIN, HIGH);
      } else {
        Serial.println("⚠ Menunggu koneksi WiFi...");
        blinkLED(1, 50);
      }
    }
  }

  delay(20);
  yield(); // penting buat WDT
}

/* ================== LOGIC UTAMA: AUTO MODE ================== */

void sendScanAuto(const String &uidHex) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AUTO] WiFi tidak terhubung, batal kirim ke Supabase");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient http;
  http.setReuse(false);

  Serial.println("[AUTO] Kirim ke RPC absensi_rfid_auto...");

  if (!http.begin(client, SUPABASE_RPC_URL)) {
    Serial.println("[AUTO] Gagal http.begin()");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  // Susun body JSON
  StaticJsonDocument<200> docPayload;
  docPayload["p_card_uid"]  = uidHex;
  docPayload["p_device_id"] = DEVICE_ID;

  String requestBody;
  serializeJson(docPayload, requestBody);

  Serial.print("[AUTO] Payload: ");
  Serial.println(requestBody);

  int httpCode = http.POST(requestBody);
  String response = http.getString();

  // Tutup koneksi Supabase dulu (hemat RAM, baru lanjut WA)
  http.end();
  client.stop();
  delay(50);
  yield();

  Serial.printf("[AUTO] HTTP code: %d\n", httpCode);
  Serial.print("[AUTO] Response: ");
  Serial.println(response);

  if (httpCode >= 200 && httpCode < 300) {
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
      Serial.print("[AUTO] JSON Error: ");
      Serial.println(error.f_str());
      blinkLED(5, 80);
      return;
    }

    bool success = doc["success"] | false;
    if (success) {
      String nama        = doc["nama"].as<String>();
      String kelas       = doc["kelas"].as<String>();
      String mapel       = doc["mapel"].as<String>();
      String status      = doc["status"].as<String>();
      String jamMulai    = doc["jam_mulai"].as<String>();
      String jamSelesai  = doc["jam_selesai"].as<String>();
      String waktu       = doc["waktu_absen"].as<String>();
      String hpWali      = doc["no_hp_wali"].as<String>();
      String hpSiswa     = doc["no_hp_siswa"].as<String>();
      String deviceId    = doc["device_id"].as<String>();
      String cardUidResp = doc["card_uid"].as<String>();
      long   absenId     = doc["absen_id"] | 0;

      Serial.println();
      Serial.println("========== HASIL ABSENSI OTOMATIS ==========");
      Serial.print("Absensi ID : "); Serial.println(absenId);
      Serial.print("UID Kartu  : "); Serial.println(cardUidResp.length() ? cardUidResp : uidHex);
      Serial.print("Nama       : "); Serial.println(nama);
      Serial.print("Kelas      : "); Serial.println(kelas);
      Serial.print("Mapel      : "); Serial.println(mapel);
      Serial.print("Status     : "); Serial.println(status);
      Serial.print("Jam Mapel  : "); Serial.print(jamMulai);
      Serial.print(" - ");
      Serial.println(jamSelesai);
      Serial.print("Waktu DB   : "); Serial.println(waktu);
      Serial.print("Device     : "); Serial.println(deviceId.length() ? deviceId : DEVICE_ID);
      Serial.print("HP Wali    : "); Serial.println(hpWali);
      Serial.print("HP Siswa   : "); Serial.println(hpSiswa);
      Serial.println("============================================");

      // Berhasil absen → indikator
      beepAndBlink(2);

      // Normalisasi nomor WA
      String targetWa = normalizePhone62(hpWali);
      if (targetWa.length() < 5) {
        targetWa = normalizePhone62(DEFAULT_WA_TUJUAN);
      }

      // Gunakan cardUid dari DB jika ada
      String cardForMsg = cardUidResp.length() ? cardUidResp : uidHex;
      String deviceForMsg = deviceId.length() ? deviceId : DEVICE_ID;

      // Kirim WhatsApp
      sendWhatsAppNotif(
        targetWa,
        nama,
        kelas,
        mapel,
        status,
        waktu,
        deviceForMsg,
        cardForMsg
      );

    } else {
      String reason  = doc["reason"].as<String>();
      String message = doc["message"].as<String>();
      String nama    = doc["nama"].as<String>();
      String kelas   = doc["kelas"].as<String>();

      Serial.println();
      Serial.println("======== SCAN TIDAK DIABSENI (AUTO) ========");
      Serial.print("UID Kartu : "); Serial.println(uidHex);
      if (nama.length()) {
        Serial.print("Nama      : "); Serial.println(nama);
        Serial.print("Kelas     : "); Serial.println(kelas);
      }
      Serial.print("Reason    : "); Serial.println(reason);
      Serial.print("Message   : "); Serial.println(message);
      Serial.println("=============================================");

      if (reason == "no_schedule_now") {
        // Di luar jam pelajaran
        beepAndBlink(4);
      } else if (reason == "rfid_not_registered") {
        // Kartu belum terdaftar
        longBeep(2500);
      } else {
        // Reason lain
        blinkLED(3, 150);
      }
    }
  } else {
    Serial.printf("[AUTO] ❌ HTTP Error: %d\n", httpCode);
    blinkLED(5, 80);
  }
}

/* ================== LOGIC: KIRIM WHATSAPP ================== */

void sendWhatsAppNotif(
  const String &noWa,
  const String &nama,
  const String &kelas,
  const String &mapel,
  const String &status,
  const String &waktuAbsen,
  const String &deviceId,
  const String &cardUid
) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MPWA] WiFi tidak terhubung, batal kirim WA");
    return;
  }

  Serial.println("[MPWA] Mempersiapkan kirim WA...");
  Serial.print("[MPWA] Target nomor: ");
  Serial.println(noWa);
  Serial.print("[MPWA] WiFi RSSI: ");
  Serial.println(WiFi.RSSI());

  WiFiClientSecure client;
  client.setInsecure();          // HTTPS tanpa verifikasi cert
  client.setTimeout(15000);
  client.setBufferSizes(512, 512); // Hemat RAM

  HTTPClient http;
  http.setReuse(false);

  if (!http.begin(client, MPWA_API_URL)) {
    Serial.println("[MPWA] Gagal http.begin()");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP8266-Absensi/1.0");

  // Susun pesan
  String message = "ABSENSI RFID\n";
  message += "Nama  : " + nama + " (" + kelas + ")\n";
  message += "Status: " + status + "\n";
  if (mapel.length()) {
    message += "Mapel : " + mapel + "\n";
  }
  if (waktuAbsen.length()) {
    message += "Waktu : " + waktuAbsen + "\n";
  }
  message += "UID   : " + cardUid + "\n";
  message += "Device: " + deviceId;

  // JSON body
  StaticJsonDocument<768> doc;
  doc["api_key"] = MPWA_API_KEY;
  doc["sender"]  = MPWA_SENDER;
  doc["number"]  = noWa;
  doc["message"] = message;
  doc["footer"]  = MPWA_FOOTER;
  doc["full"]    = 0;  // simple response

  String jsonBody;
  serializeJson(doc, jsonBody);

  Serial.print("[MPWA] Body: ");
  Serial.println(jsonBody);

  int httpCode = http.POST(jsonBody);

  String resp;
  if (httpCode > 0) {
    resp = http.getString();
  }

  Serial.printf("[MPWA] HTTP code: %d\n", httpCode);
  Serial.print("[MPWA] Response: ");
  Serial.println(resp);

  if (httpCode <= 0) {
    Serial.printf("[MPWA] ❌ HTTP error code=%d (%s)\n",
                  httpCode,
                  http.errorToString(httpCode).c_str());
    http.end();
    client.stop();
    return;
  }

  // Optional: parse respon singkat
  StaticJsonDocument<256> respDoc;
  DeserializationError err = deserializeJson(respDoc, resp);
  if (!err) {
    bool st = respDoc["status"] | false;
    const char* msg = respDoc["msg"] | "";
    if (st) {
      Serial.print("[MPWA] ✅ Sukses: ");
      Serial.println(msg);
    } else {
      Serial.print("[MPWA] ❌ Gagal (status=false): ");
      Serial.println(msg);
    }
  } else {
    Serial.print("[MPWA] ⚠ Tidak bisa parse JSON response: ");
    Serial.println(err.f_str());
  }

  http.end();
  client.stop();
}

/* ================== LOGIC: MANUAL MODE (RAW) ================== */

void sendScanManual(const String &uidHex) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MANUAL] WiFi tidak terhubung, batal kirim RAW");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient http;
  http.setReuse(false);

  Serial.println("[MANUAL] Kirim scan RAW ke tabel rfid_scans...");

  if (!http.begin(client, SUPABASE_MANUAL_URL)) {
    Serial.println("[MANUAL] Gagal http.begin()");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  StaticJsonDocument<200> doc;
  doc["card_uid"]  = uidHex;
  doc["device_id"] = DEVICE_ID;
  doc["status"]    = "raw";

  String payload;
  serializeJson(doc, payload);

  Serial.print("[MANUAL] Payload: ");
  Serial.println(payload);

  int code = http.POST(payload);
  String resp = http.getString();

  Serial.printf("[MANUAL] HTTP code: %d\n", code);
  Serial.print("[MANUAL] Response: ");
  Serial.println(resp);

  if (code >= 200 && code < 300) {
    Serial.println("[MANUAL] ✅ RAW scan tersimpan");
    beepAndBlink(1);
  } else {
    Serial.println("[MANUAL] ❌ Gagal simpan RAW");
    blinkLED(3, 120);
  }

  http.end();
  client.stop();
}

/* ================== FUNGSI BANTUAN: MODE MANUAL ================== */

void checkManualModeFromSupabase() {
  if (millis() - lastModeCheck < MODE_CHECK_INTERVAL) return;
  lastModeCheck = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient http;
  http.setReuse(false);

  if (!http.begin(client, SUPABASE_MODE_URL)) {
    Serial.println("[Setting] Gagal http.begin() mode manual");
    return;
  }

  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  int code = http.GET();
  Serial.printf("[Setting] HTTP code mode manual: %d\n", code);

  if (code == 200) {
    String payload = http.getString();
    Serial.print("[Setting] Payload: ");
    Serial.println(payload);

    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc.is<JsonArray>() && doc.size() > 0) {
      bool newMode = doc[0]["scan_manual_enabled"] | false;
      if (newMode != manualModeEnabled) {
        manualModeEnabled = newMode;
        Serial.print("[Setting] Mode Manual: ");
        Serial.println(manualModeEnabled ? "ON" : "OFF");
      }
    } else {
      Serial.print("[Setting] JSON error: ");
      Serial.println(err.f_str());
    }
  }

  http.end();
  client.stop();
}

/* ================== FUNGSI BANTUAN: WIFI ================== */

bool connectWiFi() {
  Serial.printf("Menghubungkan ke WiFi: %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    blinkLED(1, 100);
    retry++;
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\n❌ WiFi Failed!");
    return false;
  }
}

void checkWiFiConnection() {
  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Terputus, mencoba reconnect...");
      connectWiFi();
    }
  }
}

/* ================== FUNGSI BANTUAN LAIN ================== */

String normalizePhone62(const String &raw) {
  String s = raw;
  s.trim();
  s.replace("-", "");
  s.replace(" ", "");
  s.replace("+", "");
  if (s.startsWith("0")) {
    s = "62" + s.substring(1);
  }
  return s;
}

bool isSameUid(uint8_t *uid, uint8_t uidLength) {
  if (!hasLastUid || uidLength != lastUidLength) return false;
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] != lastUid[i]) return false;
  }
  return true;
}

String uidToHexString(uint8_t *uid, uint8_t len) {
  String s = "";
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void beepAndBlink(int count) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(100);
  }
}

void blinkLED(int count, int delayTime) {
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(delayTime);
    digitalWrite(LED_PIN, HIGH);
    if (i < count - 1) delay(delayTime);
  }
}

void longBeep(int durationMs) {
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, LOW);
}
