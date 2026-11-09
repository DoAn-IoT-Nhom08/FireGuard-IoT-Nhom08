#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// ===== Cau hinh mang va dinh danh nut (sua cho tung nut) =====
const char*    WIFI_SSID = "Wokwi-GUEST";
const char*    WIFI_PASS = "";
const char*    MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const int      FLOOR_NO  = 2;        // tang
const char*    NODE_ID   = "P201";   // ma nut

// ===== Chan ket noi (khop Bang 3.3) =====
const int PIN_DHT = 15, PIN_SMOKE = 34, PIN_CO = 35;
const int PIN_BUZZ = 25, PIN_LED_G = 26, PIN_LED_Y = 27, PIN_LED_R = 14;
const int PIN_BTN = 13;

// ===== Nguong va thoi gian =====
const float T_FIXED = 57.0, RISE_DELTA = 15.0, SMOKE_MAX = 300.0, CO_MAX = 50.0;
const int   RISE_CYCLES = 3;
const uint32_t EVAL_MS = 1000, TELE_MS = 2000, DHT_MS = 2000;
const uint32_t LATCH_MS = 30000, MUTE_MS = 60000;

DHT dht(PIN_DHT, DHT22);
WiFiClient net;
PubSubClient mqtt(net);

char tTele[64], tState[64], tStatus[64], tMute[64];
float hist[RISE_CYCLES + 1];
int   histN = 0;
float temp = NAN, hum = NAN;
uint32_t lastEval = 0, lastTele = 0, lastDht = 0, lastTry = 0;
uint32_t alarmUntil = 0, muteUntil = 0, seq = 0;
int lastState = -1;                  // 0 = OK, 1 = WARN, 2 = ALARM

bool inFuture(uint32_t t) { return (int32_t)(t - millis()) > 0; }

void pushTemp(float t) {
  for (int i = 0; i < RISE_CYCLES; i++) hist[i] = hist[i + 1];
  hist[RISE_CYCLES] = t;
  if (histN < RISE_CYCLES + 1) histN++;
}
float riseOf() {
  if (histN < RISE_CYCLES + 1) return 0;
  return hist[RISE_CYCLES] - hist[0];
}

// Luat bo phieu: 0 phieu = OK, 1 phieu = WARN, tu 2 phieu = ALARM
int evaluate(float t, float smoke, float co, float rise) {
  int votes = 0;
  if (t >= T_FIXED)       votes++;   // C1: nhiet co dinh
  if (rise >= RISE_DELTA) votes++;   // C2: tang nhanh
  if (smoke > SMOKE_MAX)  votes++;   // C3: khoi
  if (co > CO_MAX)        votes++;   // C4: CO
  return votes >= 2 ? 2 : (votes == 1 ? 1 : 0);
}

void applyOutputs(int s) {
  digitalWrite(PIN_LED_G, s == 0 ? HIGH : LOW);
  digitalWrite(PIN_LED_Y, s == 1 ? HIGH : LOW);
  digitalWrite(PIN_LED_R, s == 2 ? HIGH : LOW);
  bool buzz = (s == 2) && !inFuture(muteUntil);
  if (buzz) tone(PIN_BUZZ, 2000); else noTone(PIN_BUZZ);
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  if (len > 0 && payload[0] == '1') muteUntil = millis() + MUTE_MS;  // chi de thu nghiem
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastTry < 5000) return;        // thu lai moi 5 giay
  lastTry = millis();
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (WiFi.status() != WL_CONNECTED) return;
  String cid = String("fireguard-") + NODE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  // Last Will: neu mat ket noi dot ngot, broker tu dang "offline"
  if (mqtt.connect(cid.c_str(), nullptr, nullptr, tStatus, 1, true, "offline")) {
    mqtt.publish(tStatus, "online", true);
    mqtt.subscribe(tMute, 1);
  }
}

void publishState(int s) {
  const char* names[] = {"OK", "WARN", "ALARM"};
  mqtt.publish(tState, names[s], true);          // retain = true
}

void publishTele(float smoke, float co) {
  char b[128];
  snprintf(b, sizeof(b),
           R"({"t":%.1f,"h":%.1f,"smoke":%.0f,"co":%.0f,"seq":%lu,"rssi":%d})",
           temp, isnan(hum) ? 0.0f : hum, smoke, co, (unsigned long)seq++, WiFi.RSSI());
  mqtt.publish(tTele, b);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZ, OUTPUT); pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_Y, OUTPUT); pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  dht.begin();
  snprintf(tTele,   sizeof(tTele),   "fireguard/f%d/%s/telemetry", FLOOR_NO, NODE_ID);
  snprintf(tState,  sizeof(tState),  "fireguard/f%d/%s/state",     FLOOR_NO, NODE_ID);
  snprintf(tStatus, sizeof(tStatus), "fireguard/f%d/%s/status",    FLOOR_NO, NODE_ID);
  snprintf(tMute,   sizeof(tMute),   "fireguard/cmd/%s/mute",      NODE_ID);
  connectWifi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
}

void loop() {
  uint32_t now = millis();
  ensureMqtt();
  mqtt.loop();

  if (digitalRead(PIN_BTN) == LOW) muteUntil = now + MUTE_MS;   // nut Test/Mute

  if (now - lastDht >= DHT_MS) {
    lastDht = now;
    float t = dht.readTemperature(), h = dht.readHumidity();
    if (!isnan(t)) temp = t;
    if (!isnan(h)) hum = h;
  }

  if (now - lastEval >= EVAL_MS && !isnan(temp)) {
    lastEval = now;
    float smoke = analogRead(PIN_SMOKE) * 1000.0 / 4095.0;   // quy doi tuyen tinh (mo phong)
    float co    = analogRead(PIN_CO)    * 300.0  / 4095.0;
    pushTemp(temp);
    int s = evaluate(temp, smoke, co, riseOf());
    if (s == 2) alarmUntil = now + LATCH_MS;      // chot bao dong 30 giay
    if (inFuture(alarmUntil)) s = 2;
    applyOutputs(s);                              // hoat dong ca khi mat Wi-Fi
    if (mqtt.connected() && (s != lastState || s == 2)) publishState(s);
    lastState = s;
    if (mqtt.connected() && now - lastTele >= TELE_MS) {
      lastTele = now;
      publishTele(smoke, co);
    }
    Serial.printf("%s T=%.1f smoke=%.0f co=%.0f rise=%.1f state=%d\n",
                  NODE_ID, temp, smoke, co, riseOf(), s);
  }
}
