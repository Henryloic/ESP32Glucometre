#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "mbedtls/sha256.h"

/*
  LibreSportDisplay_ESP32C3_v2.ino

  Deux corrections :
  1) Le demarrage n'attend plus l'ouverture du moniteur serie.
  2) L'age affiche est uniquement le nombre de secondes ecoulees
     depuis la derniere requete API REUSSIE.

  Affichage : valeur, tendance, age de la derniere reponse valide.
*/

// =====================================================
// 1. RESEAU WI-FI DE LA MAISON
// =====================================================
const char* WIFI_SSID     = "NOM_DU_WIFI_MAISON";
const char* WIFI_PASSWORD = "MOT_DE_PASSE_WIFI";

// =====================================================
// 2. COMPTE LIBRELINKUP
// =====================================================
const char* LIBRE_EMAIL    = "ADRESSE_DU_COMPTE_LIBRELINKUP";
const char* LIBRE_PASSWORD = "MOT_DE_PASSE_LIBRELINKUP";

String apiBaseUrl = "https://api-eu.libreview.io";
const char* API_PRODUCT = "llu.android";
const char* API_VERSION = "4.17.0";

// =====================================================
// 3. ECRAN OLED SH1106 128 x 64
// =====================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;

// =====================================================
// 4. TEMPORISATIONS
// =====================================================
constexpr unsigned long API_INTERVAL_MS          = 60000UL;
constexpr unsigned long API_RETRY_AFTER_ERROR_MS = 15000UL;
constexpr unsigned long WIFI_RETRY_MS            = 10000UL;
constexpr unsigned long SCREEN_UPDATE_MS         = 500UL;
constexpr uint16_t HTTP_TIMEOUT_MS                = 15000;

// Fraicheur d'affichage uniquement, pas des seuils medicaux.
constexpr unsigned long AGE_RECENT_SECONDS = 90;
constexpr unsigned long AGE_OLD_SECONDS    = 180;

// =====================================================
// 5. DONNEES
// =====================================================
struct GlucoseData {
  int value = -1;
  int trend = 0;
  bool valid = false;

  // Instant local de la derniere requete API reussie.
  unsigned long lastSuccessfulRequestMillis = 0;
};

GlucoseData glucose;
String authToken = "";
String patientId = "";
String accountIdHash = "";
String statusMessage = "DEMARRAGE";

unsigned long lastApiAttempt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastScreenUpdate = 0;
unsigned long currentApiInterval = 0;
bool requestInProgress = false;

// =====================================================
// 6. OUTILS
// =====================================================
String sha256(const String& input) {
  unsigned char hash[32];
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  mbedtls_sha256_update(
    &context,
    reinterpret_cast<const unsigned char*>(input.c_str()),
    input.length()
  );
  mbedtls_sha256_finish(&context, hash);
  mbedtls_sha256_free(&context);

  char result[65];
  for (int i = 0; i < 32; i++) sprintf(result + i * 2, "%02x", hash[i]);
  result[64] = '\0';
  return String(result);
}

String trendText(int trend) {
  switch (trend) {
    case 1: return "--";
    case 2: return "-";
    case 3: return "=";
    case 4: return "+";
    case 5: return "++";
    default: return "?";
  }
}

// CORRECTION 2 : age calcule uniquement depuis la derniere requete reussie.
unsigned long getRequestAgeSeconds() {
  if (!glucose.valid) return 0;
  return (millis() - glucose.lastSuccessfulRequestMillis) / 1000UL;
}

String formatAge(unsigned long seconds) {
  // Affichage en secondes jusqu'a 59 min 59 s pour voir l'incrementation.
  if (seconds < 3600UL) return String(seconds) + " s";
  return String(seconds / 60UL) + " min";
}

String normalizeRegion(String region) {
  region.toLowerCase();
  region.trim();
  return region;
}

String baseUrlForRegion(String region) {
  region = normalizeRegion(region);
  if (region == "eu")  return "https://api-eu.libreview.io";
  if (region == "eu2") return "https://api-eu2.libreview.io";
  if (region == "fr")  return "https://api-fr.libreview.io";
  if (region == "de")  return "https://api-de.libreview.io";
  if (region == "us")  return "https://api.libreview.io";
  if (region == "ca")  return "https://api-ca.libreview.io";
  if (region == "au")  return "https://api-au.libreview.io";
  if (region == "ap")  return "https://api-ap.libreview.io";
  if (region == "ae")  return "https://api-ae.libreview.io";
  if (region == "jp")  return "https://api-jp.libreview.io";
  return "";
}

String jsonString(JsonVariantConst object, const char* k1,
                  const char* k2 = nullptr, const char* k3 = nullptr) {
  if (k1 && !object[k1].isNull()) return object[k1].as<String>();
  if (k2 && !object[k2].isNull()) return object[k2].as<String>();
  if (k3 && !object[k3].isNull()) return object[k3].as<String>();
  return "";
}

int jsonInt(JsonVariantConst object, const char* k1,
            const char* k2 = nullptr, int fallback = -1) {
  if (k1 && !object[k1].isNull()) return object[k1].as<int>();
  if (k2 && !object[k2].isNull()) return object[k2].as<int>();
  return fallback;
}

// =====================================================
// 7. AFFICHAGE
// =====================================================
void drawCenteredText(const String& text, int y, const uint8_t* font) {
  display.setFont(font);
  int x = (128 - display.getUTF8Width(text.c_str())) / 2;
  if (x < 0) x = 0;
  display.drawUTF8(x, y, text.c_str());
}

void drawStartupScreen(const String& line1, const String& line2) {
  display.clearBuffer();
  drawCenteredText(line1, 27, u8g2_font_helvB12_tf);
  drawCenteredText(line2, 49, u8g2_font_helvR08_tf);
  display.sendBuffer();
}

void drawScreen() {
  display.clearBuffer();

  if (!glucose.valid) {
    drawCenteredText("---", 37, u8g2_font_logisoso28_tn);
    drawCenteredText(statusMessage, 60, u8g2_font_helvR08_tf);
    display.sendBuffer();
    return;
  }

  unsigned long age = getRequestAgeSeconds();
  drawCenteredText(String(glucose.value), 38, u8g2_font_logisoso30_tn);

  String trend = trendText(glucose.trend);
  display.setFont(u8g2_font_helvB14_tf);
  int trendWidth = display.getUTF8Width(trend.c_str());
  display.drawUTF8(126 - trendWidth, 20, trend.c_str());

  String ageText;
  if (WiFi.status() != WL_CONNECTED) {
    ageText = "HORS LIGNE " + formatAge(age);
  } else if (age <= AGE_RECENT_SECONDS) {
    ageText = formatAge(age);
  } else if (age <= AGE_OLD_SECONDS) {
    ageText = "AGE " + formatAge(age);
  } else {
    ageText = "ANCIEN " + formatAge(age);
  }

  drawCenteredText(ageText, 61, u8g2_font_helvB08_tf);

  if (age > AGE_OLD_SECONDS) {
    display.drawFrame(0, 0, 128, 64);
    display.drawFrame(2, 2, 124, 60);
  }

  display.sendBuffer();
}

// =====================================================
// 8. WI-FI
// =====================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  statusMessage = "CONNEXION WIFI";
  drawStartupScreen("Wi-Fi", WIFI_SSID);
  Serial.print("Connexion Wi-Fi : ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    statusMessage = "WIFI OK";
    Serial.print("Wi-Fi OK, IP : ");
    Serial.println(WiFi.localIP());
    drawStartupScreen("Wi-Fi OK", WiFi.localIP().toString());
  } else {
    statusMessage = "WIFI ABSENT";
    Serial.println("Wi-Fi absent");
    drawStartupScreen("Wi-Fi absent", "Nouvel essai");
  }
}

// =====================================================
// 9. EN-TETES HTTPS
// =====================================================
void configureCommonHeaders(HTTPClient& http) {
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("product", API_PRODUCT);
  http.addHeader("version", API_VERSION);
  http.addHeader("Cache-Control", "no-cache");
}

void configureAuthenticatedHeaders(HTTPClient& http) {
  configureCommonHeaders(http);
  http.addHeader("Authorization", "Bearer " + authToken);
  if (accountIdHash.length() > 0) http.addHeader("Account-Id", accountIdHash);
}

// =====================================================
// 10. AUTHENTIFICATION
// =====================================================
bool authenticateLibreLinkUp() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // Deux passages pour prendre en charge une redirection regionale.
  for (int attempt = 0; attempt < 2; attempt++) {
    statusMessage = "CONNEXION LIBRE";
    drawScreen();

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(HTTP_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    String url = apiBaseUrl + "/llu/auth/login";

    Serial.print("Login : ");
    Serial.println(url);

    if (!http.begin(client, url)) {
      statusMessage = "HTTPS IMPOSSIBLE";
      return false;
    }

    configureCommonHeaders(http);

    JsonDocument request;
    request["email"] = LIBRE_EMAIL;
    request["password"] = LIBRE_PASSWORD;
    String body;
    serializeJson(request, body);

    int code = http.POST(body);
    String response = http.getString();
    http.end();

    Serial.print("Login HTTP : ");
    Serial.println(code);

    if (code < 200 || code >= 300) {
      statusMessage = "LOGIN HTTP " + String(code);
      return false;
    }

    JsonDocument json;
    if (deserializeJson(json, response)) {
      statusMessage = "JSON LOGIN";
      return false;
    }

    bool redirect = json["data"]["redirect"] | false;
    String region = json["data"]["region"] | "";

    if (redirect || region.length() > 0) {
      String newUrl = baseUrlForRegion(region);
      Serial.print("Redirection region : ");
      Serial.println(region);
      if (newUrl.length() == 0 || newUrl == apiBaseUrl) {
        statusMessage = "REGION " + region;
        return false;
      }
      apiBaseUrl = newUrl;
      continue;
    }

    authToken = json["data"]["authTicket"]["token"] | "";
    if (authToken.length() == 0) authToken = json["data"]["token"] | "";
    if (authToken.length() == 0) authToken = json["authTicket"]["token"] | "";

    String accountId = json["data"]["user"]["id"] | "";

    if (authToken.length() == 0) {
      statusMessage = "TOKEN ABSENT";
      Serial.println("Token absent");
      return false;
    }

    accountIdHash = accountId.length() ? sha256(accountId) : "";
    statusMessage = "LOGIN OK";
    Serial.println("Authentification OK");
    return true;
  }

  return false;
}

// =====================================================
// 11. PATIENT
// =====================================================
bool fetchPatientId() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = apiBaseUrl + "/llu/connections";

  if (!http.begin(client, url)) return false;
  configureAuthenticatedHeaders(http);

  int code = http.GET();
  String response = http.getString();
  http.end();

  Serial.print("Connections HTTP : ");
  Serial.println(code);

  if (code == 401) {
    authToken = "";
    patientId = "";
    return false;
  }
  if (code < 200 || code >= 300) return false;

  JsonDocument json;
  if (deserializeJson(json, response)) return false;

  JsonVariantConst first = json["data"][0];
  patientId = jsonString(first, "patientId", "id");

  if (patientId.length() == 0) {
    statusMessage = "PATIENT ABSENT";
    return false;
  }

  Serial.println("Patient OK");
  return true;
}

// =====================================================
// 12. MESURE
// =====================================================
bool fetchLatestMeasurement() {
  if (WiFi.status() != WL_CONNECTED) {
    statusMessage = "WIFI ABSENT";
    return false;
  }

  if (authToken.length() == 0 && !authenticateLibreLinkUp()) return false;
  if (patientId.length() == 0 && !fetchPatientId()) return false;

  statusMessage = "LECTURE";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = apiBaseUrl + "/llu/connections/" + patientId + "/graph";

  if (!http.begin(client, url)) return false;
  configureAuthenticatedHeaders(http);

  int code = http.GET();
  String response = http.getString();
  http.end();

  Serial.print("Mesure HTTP : ");
  Serial.println(code);

  if (code == 401) {
    authToken = "";
    patientId = "";
    statusMessage = "SESSION EXPIREE";
    return false;
  }
  if (code < 200 || code >= 300) {
    statusMessage = "ERREUR SERVEUR";
    return false;
  }

  JsonDocument json;
  if (deserializeJson(json, response)) {
    statusMessage = "JSON INVALIDE";
    return false;
  }

  JsonVariantConst measurement = json["data"]["connection"]["glucoseMeasurement"];
  if (measurement.isNull()) {
    measurement = json["data"]["connection"]["glucoseItem"];
  }
  if (measurement.isNull()) {
    statusMessage = "MESURE ABSENTE";
    return false;
  }

  int newValue = jsonInt(measurement, "Value", "value", -1);
  int newTrend = jsonInt(measurement, "TrendArrow", "trend", 0);

  if (newValue < 0) {
    statusMessage = "VALEUR ABSENTE";
    return false;
  }

  // Mise a jour uniquement apres une reponse valide.
  glucose.value = newValue;
  glucose.trend = newTrend;
  glucose.valid = true;
  glucose.lastSuccessfulRequestMillis = millis(); // Age remis a zero ici.
  statusMessage = "LIVE";

  Serial.print("Valeur : ");
  Serial.println(glucose.value);
  Serial.print("Tendance : ");
  Serial.println(trendText(glucose.trend));
  Serial.println("Age de requete remis a 0 s");

  return true;
}

// =====================================================
// 13. GESTION PERIODIQUE
// =====================================================
void manageWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  if (lastWifiAttempt == 0 || millis() - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = millis();
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      lastApiAttempt = 0;
      currentApiInterval = 0;
    }
  }
}

void manageApi() {
  if (WiFi.status() != WL_CONNECTED || requestInProgress) return;

  bool firstRequest = (lastApiAttempt == 0);
  bool elapsed = (currentApiInterval == 0) ||
                 (millis() - lastApiAttempt >= currentApiInterval);
  if (!firstRequest && !elapsed) return;

  requestInProgress = true;
  lastApiAttempt = millis();

  Serial.println("Interrogation LibreLinkUp");
  bool success = fetchLatestMeasurement();

  if (success) {
    currentApiInterval = API_INTERVAL_MS;
    Serial.println("Prochaine requete dans 60 s");
  } else {
    currentApiInterval = API_RETRY_AFTER_ERROR_MS;
    Serial.println("Echec, nouvel essai dans 15 s");
  }

  requestInProgress = false;
}

// =====================================================
// 14. SETUP
// =====================================================
void setup() {
  Serial.begin(115200);

  // CORRECTION 1 : aucune attente de l'ouverture du port serie.
  // L'ESP32 demarre donc aussi avec un simple chargeur USB.
  delay(500);

  Serial.println();
  Serial.println("ESP32-C3 GLUCOSE MONITOR");
  Serial.println("Demarrage autonome");

  Wire.begin(I2C_SDA, I2C_SCL);
  delay(500);
  display.begin();
  display.setContrast(255);
  drawStartupScreen("Libre Sport", "Demarrage");

  connectWiFi();

  lastApiAttempt = 0;
  currentApiInterval = 0;
}

// =====================================================
// 15. LOOP
// =====================================================
void loop() {
  manageWiFi();
  manageApi();

  if (millis() - lastScreenUpdate >= SCREEN_UPDATE_MS) {
    lastScreenUpdate = millis();
    drawScreen();
  }

  delay(20);
}
