#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SD.h>
#include <Wire.h>
#include <VL53L0X.h>
#include "Audio.h"
#include <ArduinoJson.h>

// --- ПІНИ ---
#define SD_CS 5
#define I2S_LRC 25
#define I2S_BCLK 27
#define I2S_DOUT 26
#define BTN_RESET 0 // Кнопка BOOT на платі для Hard Reset

// --- ОБ'ЄКТИ ---
AsyncWebServer server(80);
Preferences prefs;
Audio audio;
VL53L0X sensor;

// --- СТАН ---
String adminUser = "admin";
String adminPass = "admin";
String wifiSSID = "";
String wifiPass = "";
String triggerTrack = "/1.mp3";
bool isPlaylistMode = false;
bool isAPMode = false;

unsigned long lastSensorCheck = 0;
unsigned long btnPressTime = 0;
const int TRIGGER_DISTANCE = 200; // 200 мм = 20 см

// --- ФУНКЦІЯ ЗАВАНТАЖЕННЯ ФАЙЛІВ НА SD ---
void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    if (!filename.startsWith("/")) filename = "/" + filename;
    Serial.printf("Початок завантаження: %s\n", filename.c_str());
    request->_tempFile = SD.open(filename, FILE_WRITE);
  }
  if (request->_tempFile) {
    if (len) request->_tempFile.write(data, len);
    if (final) {
      request->_tempFile.close();
      Serial.printf("Завантажено: %s, Розмір: %u B\n", filename.c_str(), index + len);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_RESET, INPUT_PULLUP);
  
  // 1. Ініціалізація пам'яті налаштувань
  prefs.begin("storyframe", false);
  adminUser = prefs.getString("user", "admin");
  adminPass = prefs.getString("pass", "admin");
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("wifipass", "");
  triggerTrack = prefs.getString("track", "/1.mp3");
  isPlaylistMode = prefs.getBool("playlist", false);

  // 2. Ініціалізація LittleFS (для Web UI)
  if(!LittleFS.begin(true)) {
    Serial.println("Помилка монтування LittleFS");
    return;
  }

  // 3. Ініціалізація SD-карти
  if(!SD.begin(SD_CS)) Serial.println("Помилка SD-карти (але працюємо далі)");

  // 4. Ініціалізація Аудіо
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(15);

  // 5. Ініціалізація Датчика
  Wire.begin();
  sensor.setTimeout(500);
  if (sensor.init()) {
    sensor.startContinuous(100);
    Serial.println("Датчик VL53L0X готовий");
  }

  // 6. Мережа (Wi-Fi або AP)
  if (wifiSSID != "") {
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    Serial.print("Підключення до Wi-Fi");
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500); Serial.print("."); retries++;
    }
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nЗапуск Точки Доступу (AP Mode)");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("StoryFrame_Setup", "12345678"); // Пароль від точки доступу пристрою
    isAPMode = true;
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("\nПідключено! IP: " + WiFi.localIP().toString());
  }

  // ================= API ENDPOINTS =================

  // Головна сторінка адмінки
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if(!request->authenticate(adminUser.c_str(), adminPass.c_str())) return request->requestAuthentication();
    request->send(LittleFS, "/index.html", "text/html");
  });

  // Отримання списку mp3 файлів з SD карти
  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *request){
    if(!request->authenticate(adminUser.c_str(), adminPass.c_str())) return request->send(401);
    JsonDocument doc;
    JsonArray files = doc["files"].to<JsonArray>();
    File root = SD.open("/");
    File file = root.openNextFile();
    while(file){
      if(!file.isDirectory() && String(file.name()).endsWith(".mp3")){
        files.add(String("/") + file.name());
      }
      file = root.openNextFile();
    }
    String res; serializeJson(doc, res);
    request->send(200, "application/json", res);
  });

  // Завантаження файлу
  server.on("/api/upload", HTTP_POST, [](AsyncWebServerRequest *request){
    if(!request->authenticate(adminUser.c_str(), adminPass.c_str())) return request->send(401);
    request->send(200, "text/plain", "OK");
  }, handleUpload);

  // Збереження налаштувань
  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request){
    if(!request->authenticate(adminUser.c_str(), adminPass.c_str())) return request->send(401);
    
    if(request->hasParam("ssid", true)) prefs.putString("ssid", request->getParam("ssid", true)->value());
    if(request->hasParam("wifipass", true)) prefs.putString("wifipass", request->getParam("wifipass", true)->value());
    if(request->hasParam("track", true)) {
      triggerTrack = request->getParam("track", true)->value();
      prefs.putString("track", triggerTrack);
    }
    if(request->hasParam("playlist", true)) {
      isPlaylistMode = (request->getParam("playlist", true)->value() == "1");
      prefs.putBool("playlist", isPlaylistMode);
    }
    request->send(200, "text/plain", "Saved");
  });

  // Скидання налаштувань (Soft Reset)
  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request){
    if(!request->authenticate(adminUser.c_str(), adminPass.c_str())) return request->send(401);
    request->send(200, "text/plain", "Rebooting...");
    delay(1000);
    prefs.clear();
    ESP.restart();
  });

  server.begin();
}

// Функція для плейлиста (відтворення наступного файлу)
void playNextInPlaylist() {
  File root = SD.open("/");
  File file = root.openNextFile();
  while(file){
    String fname = String("/") + file.name();
    if(!file.isDirectory() && fname.endsWith(".mp3") && fname != triggerTrack) {
      triggerTrack = fname; // Тимчасово змінюємо поточний трек
      audio.connecttoFS(SD, triggerTrack.c_str());
      return;
    }
    file = root.openNextFile();
  }
}

void loop() {
  audio.loop();

  // Логіка плейлиста: якщо трек закінчився і увімкнено режим плейлиста
  if (isPlaylistMode && !audio.isRunning() && SD.exists(triggerTrack.c_str())) {
      // Тут можна додати логіку автоматичного старту наступного треку, 
      // але для інтерактивної рамки краще, щоб плейлист прокручувався ПРИ кожному спрацьовуванні датчика.
  }

  // Hard Reset кнопкою BOOT (затиснути на 5 секунд)
  if (digitalRead(BTN_RESET) == LOW) {
    if (btnPressTime == 0) btnPressTime = millis();
    if (millis() - btnPressTime > 5000) {
      Serial.println("HARD RESET!");
      prefs.clear();
      ESP.restart();
    }
  } else {
    btnPressTime = 0;
  }

  // Опитування датчика (тільки якщо аудіо не грає)
  if (!audio.isRunning() && millis() - lastSensorCheck > 150) {
    lastSensorCheck = millis();
    uint16_t dist = sensor.readRangeContinuousMillimeters();
    if (!sensor.timeoutOccurred() && dist < TRIGGER_DISTANCE) {
      Serial.printf("Тригер! Відстань: %d мм\n", dist);
      if (SD.exists(triggerTrack.c_str())) {
        audio.connecttoFS(SD, triggerTrack.c_str());
      }
      if (isPlaylistMode) {
        playNextInPlaylist(); // Готуємо наступний трек для наступного спрацьовування
      }
    }
  }
}