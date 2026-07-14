#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>

#define WIFI_AP_NAME "D1mini-Meteo" // point d'acces de config au 1er demarrage
#define MDNS_HOSTNAME "meteo" // accessible ensuite via http://meteo.local

ESP8266WebServer server(80);

// D1 mini I2C par defaut : SDA = D2 (GPIO4), SCL = D1 (GPIO5)
#define I2C_SDA D2
#define I2C_SCL D1
#define I2C_CLOCK_HZ 400000 // mode rapide I2C : rafraichissement OLED plus court

#define SCREEN_WIDTH 64
#define SCREEN_HEIGHT 48
#define OLED_RESET -1
#define OLED_I2C_ADDRESS 0x3C
#define CANVAS_BYTES (((SCREEN_WIDTH + 7) / 8) * SCREEN_HEIGHT)

// Le BME280 est empile directement sur le D1 mini : la puce ESP8266 et le
// regulateur de tension chauffent juste en dessous, ce qui fausse la mesure
// de temperature. Cet offset compense approximativement cet auto-echauffement
// (mesure : 28.8 C affiches pour 22.5 C reels). A reajuster si besoin.
#define TEMP_OFFSET_C -6.3f

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_BME280 bme;
bool bmeOk = false;

// Chaque ecran est pre-rendu une seule fois dans un canevas hors-ecran :
// l'animation se contente ensuite de faire glisser ces bitmaps tels quels,
// sans recalculer de texte a chaque frame (evite tout artefact visuel
// pendant le mouvement, et allege largement le CPU).
GFXcanvas1 canvasCur(SCREEN_WIDTH, SCREEN_HEIGHT);
GFXcanvas1 canvasNext(SCREEN_WIDTH, SCREEN_HEIGHT);

struct Reading {
  float temperature;
  float humidity;
  float pressure;
};
Reading reading = {0, 0, 0};

enum ScreenId { SCREEN_TEMP, SCREEN_HUM, SCREEN_PRESS, SCREEN_COUNT };

const uint32_t SENSOR_READ_INTERVAL_MS = 2000;
const uint32_t SCREEN_HOLD_MS = 3000;
const uint32_t SLIDE_DURATION_MS = 350;

uint32_t lastSensorRead = 0;
uint32_t screenChangeAt = 0;
uint8_t currentScreen = SCREEN_TEMP;
bool sliding = false;
uint32_t slideStart = 0;
bool curDirty = true;

// Historique en RAM (buffer circulaire) pour le graphique de la page web :
// un point par minute, 60 points -> 1h glissante. Perdu au redemarrage,
// ce qui est suffisant pour un usage domestique.
#define HISTORY_SIZE 60
const uint32_t HISTORY_INTERVAL_MS = 60000;

float histTemp[HISTORY_SIZE];
float histHum[HISTORY_SIZE];
float histPress[HISTORY_SIZE];
uint16_t histCount = 0;
uint16_t histHead = 0;
uint32_t lastHistoryPush = 0;

void pushHistory() {
  histTemp[histHead] = reading.temperature;
  histHum[histHead] = reading.humidity;
  histPress[histHead] = reading.pressure;
  histHead = (histHead + 1) % HISTORY_SIZE;
  if (histCount < HISTORY_SIZE) histCount++;
}

bool initBme() {
  if (bme.begin(0x76)) return true;
  if (bme.begin(0x77)) return true;
  return false;
}

void readSensor() {
  if (!bmeOk) return;
  reading.temperature = bme.readTemperature() + TEMP_OFFSET_C;
  reading.humidity = bme.readHumidity();
  reading.pressure = bme.readPressure() / 100.0F;
  curDirty = true;
}

const char *screenLabel(uint8_t s) {
  switch (s) {
    case SCREEN_TEMP: return "TEMP";
    case SCREEN_HUM: return "HUMIDITE";
    case SCREEN_PRESS: return "PRESSION";
    default: return "";
  }
}

void formatValue(uint8_t s, char *out, size_t outSize, const char **unit) {
  switch (s) {
    case SCREEN_TEMP:
      snprintf(out, outSize, "%.1f", reading.temperature);
      *unit = "C";
      break;
    case SCREEN_HUM:
      snprintf(out, outSize, "%.0f", reading.humidity);
      *unit = "%";
      break;
    case SCREEN_PRESS:
      snprintf(out, outSize, "%.0f", reading.pressure);
      *unit = "hPa";
      break;
    default:
      out[0] = '\0';
      *unit = "";
  }
}

template <typename T>
void drawCenteredOn(T &gfx, const char *text, uint8_t textSize, int16_t xOffset, int16_t y) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx.setTextSize(textSize);
  gfx.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  gfx.setCursor(xOffset + (SCREEN_WIDTH - (int16_t)w) / 2, y);
  gfx.print(text);
}

// Icones dessinees a la main (pas de bitmap) : largeur/hauteur approximatives
// pour pouvoir centrer le bloc icone+valeur.
uint8_t iconWidth(uint8_t screenId) {
  switch (screenId) {
    case SCREEN_TEMP: return 9;
    case SCREEN_HUM: return 10;
    case SCREEN_PRESS: return 14;
    default: return 0;
  }
}

void drawIcon(GFXcanvas1 &canvas, uint8_t screenId, int16_t x, int16_t y) {
  switch (screenId) {
    case SCREEN_TEMP: // thermometre : tige + bulbe
      canvas.fillRoundRect(x + 3, y, 3, 10, 1, SSD1306_WHITE);
      canvas.fillCircle(x + 4, y + 11, 4, SSD1306_WHITE);
      break;
    case SCREEN_HUM: // goutte d'eau
      canvas.fillTriangle(x + 5, y + 2, x, y + 10, x + 10, y + 10, SSD1306_WHITE);
      canvas.fillCircle(x + 5, y + 10, 5, SSD1306_WHITE);
      break;
    case SCREEN_PRESS: // jauge / cadran avec aiguille
      canvas.drawCircle(x + 6, y + 9, 7, SSD1306_WHITE);
      canvas.drawLine(x + 6, y + 9, x + 10, y + 4, SSD1306_WHITE);
      canvas.fillCircle(x + 6, y + 9, 1, SSD1306_WHITE);
      break;
  }
}

void renderScreen(GFXcanvas1 &canvas, uint8_t screenId) {
  char valueStr[12];
  const char *unit;
  formatValue(screenId, valueStr, sizeof(valueStr), &unit);

  canvas.fillScreen(SSD1306_BLACK);
  canvas.setTextColor(SSD1306_WHITE);
  canvas.drawRoundRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 8, SSD1306_WHITE);

  drawCenteredOn(canvas, screenLabel(screenId), 1, 0, 5);

  // Bloc icone + grande valeur, centre comme un seul ensemble.
  canvas.setTextSize(2);
  int16_t x1, y1;
  uint16_t w, h;
  canvas.getTextBounds(valueStr, 0, 0, &x1, &y1, &w, &h);
  int16_t gap = 3;
  int16_t iconW = iconWidth(screenId);
  int16_t blockW = iconW + gap + (int16_t)w;
  int16_t blockX = (SCREEN_WIDTH - blockW) / 2;

  drawIcon(canvas, screenId, blockX, 18);
  canvas.setCursor(blockX + iconW + gap, 20);
  canvas.print(valueStr);

  drawCenteredOn(canvas, unit, 1, 0, 37);
}

void handleData() {
  String json = "{\"temperature\":" + String(reading.temperature, 1) +
                ",\"humidity\":" + String(reading.humidity, 0) +
                ",\"pressure\":" + String(reading.pressure, 0) + "}";
  server.send(200, "application/json", json);
}

void appendHistorySeries(String &json, float *series) {
  json += "[";
  for (uint16_t i = 0; i < histCount; i++) {
    uint16_t idx = (histHead + HISTORY_SIZE - histCount + i) % HISTORY_SIZE;
    if (i) json += ",";
    json += String(series[idx], 1);
  }
  json += "]";
}

void handleHistory() {
  String json = "{\"temperature\":";
  appendHistorySeries(json, histTemp);
  json += ",\"humidity\":";
  appendHistorySeries(json, histHum);
  json += ",\"pressure\":";
  appendHistorySeries(json, histPress);
  json += "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Station meteo</title>"
      "<style>"
      "body{font-family:sans-serif;background:#111;color:#eee;"
      "display:flex;flex-direction:column;align-items:center;"
      "min-height:100vh;margin:0;gap:1.2em;padding:1.5em 0}"
      "h1{font-size:1.1em;color:#888;font-weight:normal;margin:0}"
      ".row{display:flex;gap:2em}"
      ".card{background:#1c1c1c;border-radius:12px;padding:1em 1.5em;text-align:center}"
      ".label{color:#888;font-size:.8em;text-transform:uppercase}"
      ".value{font-size:2em;font-weight:bold}"
      ".charts{display:flex;flex-direction:column;gap:.8em;width:300px;max-width:90vw}"
      ".chart-label{color:#888;font-size:.75em;text-transform:uppercase;margin-bottom:.2em}"
      "canvas{background:#1c1c1c;border-radius:8px;width:100%;height:60px;display:block}"
      "</style></head><body>"
      "<h1>Station meteo D1 mini</h1><div class='row'>");
  html += "<div class='card'><div class='label'>Temperature</div><div class='value' id='vTemp'>" +
          String(reading.temperature, 1) + " C</div></div>";
  html += "<div class='card'><div class='label'>Humidite</div><div class='value' id='vHum'>" +
          String(reading.humidity, 0) + " %</div></div>";
  html += "<div class='card'><div class='label'>Pression</div><div class='value' id='vPress'>" +
          String(reading.pressure, 0) + " hPa</div></div>";
  html += F(
      "</div>"
      "<div class='charts'>"
      "<div><div class='chart-label'>Temperature (1h)</div>"
      "<canvas id='cTemp' width='300' height='60'></canvas></div>"
      "<div><div class='chart-label'>Humidite (1h)</div>"
      "<canvas id='cHum' width='300' height='60'></canvas></div>"
      "<div><div class='chart-label'>Pression (1h)</div>"
      "<canvas id='cPress' width='300' height='60'></canvas></div>"
      "</div>"
      "<script>"
      "function drawChart(id,data,color){"
      "var c=document.getElementById(id);var ctx=c.getContext('2d');"
      "var w=c.width,h=c.height;ctx.clearRect(0,0,w,h);"
      "if(!data||data.length<2)return;"
      "var min=Math.min.apply(null,data),max=Math.max.apply(null,data);"
      "if(min==max){min-=1;max+=1;}"
      "ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();"
      "data.forEach(function(v,i){"
      "var x=i/(data.length-1)*w;var y=h-((v-min)/(max-min))*(h-14)-2;"
      "if(i==0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});"
      "ctx.stroke();"
      "ctx.fillStyle='#888';ctx.font='10px sans-serif';"
      "ctx.fillText(max.toFixed(1),4,10);"
      "ctx.fillText(min.toFixed(1),4,h-3);}"
      "function refreshAll(){"
      "fetch('/data').then(function(r){return r.json();}).then(function(d){"
      "document.getElementById('vTemp').textContent=d.temperature.toFixed(1)+' C';"
      "document.getElementById('vHum').textContent=d.humidity.toFixed(0)+' %';"
      "document.getElementById('vPress').textContent=d.pressure.toFixed(0)+' hPa';});"
      "fetch('/history').then(function(r){return r.json();}).then(function(d){"
      "drawChart('cTemp',d.temperature,'#4fc3f7');"
      "drawChart('cHum',d.humidity,'#81c784');"
      "drawChart('cPress',d.pressure,'#ffb74d');});}"
      "refreshAll();setInterval(refreshAll,5000);"
      "</script>"
      "</body></html>");
  server.send(200, "text/html", html);
}

void drawCloudIcon(int16_t cx, int16_t topY) {
  int16_t x = cx - 9;
  display.fillCircle(x + 4, topY + 6, 4, SSD1306_WHITE);
  display.fillCircle(x + 9, topY + 3, 5, SSD1306_WHITE);
  display.fillCircle(x + 15, topY + 5, 4, SSD1306_WHITE);
  display.fillRoundRect(x, topY + 5, 19, 5, 2, SSD1306_WHITE);
}

void showSplashScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.drawRoundRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 8, SSD1306_WHITE);

  drawCloudIcon(SCREEN_WIDTH / 2, 2);

  drawCenteredOn(display, "METEO", 1, 0, 13);
  drawCenteredOn(display, "STATION", 1, 0, 22);
  drawCenteredOn(display, "V1.0.0", 1, 0, 31);
  drawCenteredOn(display, "by Cypher", 1, 0, 39);

  display.display();
  delay(2500);
}

void showWifiConfigScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.drawRoundRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 8, SSD1306_WHITE);

  // Icone WiFi : point + deux arcs (cercles dont on masque la moitie basse).
  int16_t cx = SCREEN_WIDTH / 2;
  int16_t cy = 10;
  display.fillCircle(cx, cy, 2, SSD1306_WHITE);
  display.drawCircle(cx, cy, 5, SSD1306_WHITE);
  display.drawCircle(cx, cy, 9, SSD1306_WHITE);
  display.fillRect(cx - 9, cy + 1, 19, 9, SSD1306_BLACK);

  drawCenteredOn(display, "CONNECT:", 1, 0, 15);
  drawCenteredOn(display, "D1mini-", 1, 0, 26);
  drawCenteredOn(display, "Meteo", 1, 0, 36);
  display.display();
}

void onWifiConfigMode(WiFiManager *wm) {
  (void)wm;
  showWifiConfigScreen();
}

void setupNetwork() {
  WiFiManager wifiManager;
  wifiManager.setAPCallback(onWifiConfigMode);
  wifiManager.autoConnect(WIFI_AP_NAME);

  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.println("mDNS actif : http://" MDNS_HOSTNAME ".local");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.begin();
  Serial.print("Serveur HTTP demarre, IP : ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println("Erreur : ecran OLED non detecte");
  }

  // Ce panneau 64x48 n'est pas nativement supporte par Adafruit_SSD1306 :
  // la config COM par defaut (0x02) superpose les lignes du bas sur celles
  // du haut. 0x12 (mapping alterne) est la valeur qui fonctionne sur ce module.
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  Wire.write(0x00); // control byte : commande
  Wire.write(0xDA); // SET COM PINS
  Wire.write(0x12);
  Wire.endTransmission();

  display.clearDisplay();
  display.display();

  showSplashScreen();

  bmeOk = initBme();
  if (!bmeOk) {
    Serial.println("Erreur : BME280 non detecte (adresses 0x76/0x77)");
  } else {
    readSensor();
    pushHistory();
  }

  renderScreen(canvasCur, currentScreen);
  curDirty = false;

  setupNetwork();

  uint32_t now = millis();
  lastSensorRead = now;
  lastHistoryPush = now;
  screenChangeAt = now + SCREEN_HOLD_MS;
}

void loop() {
  server.handleClient();
  MDNS.update();

  uint32_t now = millis();

  if (bmeOk && now - lastSensorRead >= SENSOR_READ_INTERVAL_MS) {
    lastSensorRead = now;
    readSensor();
  }

  if (bmeOk && now - lastHistoryPush >= HISTORY_INTERVAL_MS) {
    lastHistoryPush = now;
    pushHistory();
  }

  if (!bmeOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(4, 16);
    display.print("BME280");
    display.setCursor(4, 28);
    display.print("absent");
    display.display();
    return;
  }

  if (curDirty && !sliding) {
    renderScreen(canvasCur, currentScreen);
    curDirty = false;
  }

  if (!sliding && now >= screenChangeAt) {
    sliding = true;
    slideStart = now;
    renderScreen(canvasNext, (currentScreen + 1) % SCREEN_COUNT);
  }

  display.clearDisplay();

  if (sliding) {
    uint32_t elapsed = now - slideStart;
    if (elapsed >= SLIDE_DURATION_MS) {
      sliding = false;
      currentScreen = (currentScreen + 1) % SCREEN_COUNT;
      screenChangeAt = now + SCREEN_HOLD_MS;
      memcpy(canvasCur.getBuffer(), canvasNext.getBuffer(), CANVAS_BYTES);
      display.drawBitmap(0, 0, canvasCur.getBuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    } else {
      int16_t offset = (int16_t)(((uint32_t)SCREEN_WIDTH * elapsed) / SLIDE_DURATION_MS);
      display.drawBitmap(-offset, 0, canvasCur.getBuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
      display.drawBitmap(SCREEN_WIDTH - offset, 0, canvasNext.getBuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    }
  } else {
    display.drawBitmap(0, 0, canvasCur.getBuffer(), SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  }

  display.display();
}
