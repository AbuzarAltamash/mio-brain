// EMO ESP32-S3 - Direct AI Connection & HTTPS Server
// Libraries Required: 
// 1. Adafruit GFX, Adafruit ST7735, Adafruit NeoPixel
// 2. ArduinoJson
// 3. esp32_https_server (by Frank Hessel)

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <math.h>

// HTTPS Server Includes
#include <HTTPSServer.hpp>
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>

using namespace httpsserver;

// --- CONFIGURATION ---
const char* homeSSID = "Appsphero Technologies";
const char* homePassword = "checking1234";

// Replace with your actual API endpoint URL (e.g., your Ngrok URL + /api/chat)
const char* aiServerUrl = "https://ollama.com/api/chat";
const char* aiModel = "gemma4:cloud";
const char* apiKey = "8c29b30c4415491d9fda6a4b14e989df.fP5vKN-5zKCKDdoM3qXkr3kM"; // Added to Authorization header

#define TFT_CLK 14
#define TFT_MOSI 21
#define TFT_CS 47
#define TFT_DC 40
#define ST77XX_ORANGE 0xFD20
#define TFT_RST 38
#define TFT_BL 15
#define RGB_PIN 48
#define RGB_COUNT 1

Adafruit_ST7735 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
Adafruit_NeoPixel rgb(RGB_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);

SSLCert * cert;
HTTPSServer * secureServer;

enum State { IDLE, LISTENING, THINKING, SPEAKING, ERROR_STATE };
volatile State state = IDLE;
volatile bool aiBusy = false, resultReady = false, aiSuccess = false;

String pendingCommand = "", aiReply = "", aiAction = "none", aiMode = "solid", aiScreen = "none";
int aiR = 0, aiG = 0, aiB = 0, aiSpeed = 120, aiBrightness = 255;
int lightR = 0, lightG = 0, lightB = 0, lightBrightness = 255;
String lightMode = "solid";
int lightSpeed = 120;
unsigned long lightTick = 0;
bool blinkOn = true;
int breatheValue = 0, breatheDir = 1;
String screenMode = "none";
unsigned long screenTick = 0;
int dogFrame = 0;
String resultReply = "";
bool resultSuccess = false;
uint32_t resultVersion = 0;
unsigned long animTick = 0;
int frame = 0;

State prevState = (State)-1;
String prevScreenMode = "";

int prevRad = 0;
int prevH[7] = {0};

// Conversational Memory (Stores last 2 exchanges = 4 messages) to save input tokens
String chatHistory[4];
int chatCount = 0;

const char* SYSTEM_PROMPT = R"PROMPT(
You are EMO, a warm, extremely friendly, and enthusiastic AI companion controlling an ESP32. Keep answers very short and conversational but no emojis or emoticons allowed. Always sound happy to help!
Understand natural language; never require exact phrases.
You can use tools to fetch real-world information.

Return ONLY one valid JSON object with exactly these keys:
{
 "reply":"short natural response",
 "action":"none or set_light",
 "r":0,"g":0,"b":0,
 "mode":"solid or blink or breathe or off",
 "speed_ms":120,
 "brightness":255,
 "screen":"none or dog_running or clear or face_idle or face_happy or face_sad or face_angry",
 "tool":"none or weather or joke",
 "tool_args":""
}

LIGHT INTERPRETATION:
- Any request about changing, turning, dimming, brightening, blinking, pulsing, breathing, fading, flashing, or smoothly turning an RGB light is action="set_light".
- Convert requested/implied colors to RGB creatively.
- "blink fast": blink with speed_ms around 80-180.
- "blink slow": around 500-1200.
- "dim": lower brightness appropriately, usually 20-100, while preserving requested/current implied color if specified.
- "bright": brightness 200-255.
- "smooth", "fade", "pulse", "breathe", "smooth on and off": mode="breathe".
- "turn off": action="set_light", mode="off", RGB 0,0,0, brightness 0.
- "turn on" without a color should use a sensible white unless context provides a color.

SCREEN INTERPRETATION:
- To show a dog running, set screen="dog_running". To clear the screen, set screen="clear".
- To show expressive robot eyes, set screen to one of: "face_idle", "face_happy", "face_sad", "face_angry". Choose the emotion that fits your reply.
- A light command and a screen request may occur together.

TOOL USAGE:
- If you need to know the current weather, set "tool":"weather" and "tool_args":"City Name". (If no city is given, leave it empty).
- If you are asked to tell a joke, set "tool":"joke".

Keep reply short. No markdown. No extra text outside JSON.
)PROMPT";

// --- DRAWING FUNCTIONS ---
void header(const char* title, uint16_t c = ST77XX_CYAN) {
  tft.fillRect(0, 0, 128, 16, c);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_BLACK);
  tft.setCursor(3, 5);
  tft.print(title);
}
void drawIdle(bool init) {
  if (init) {
    tft.fillScreen(ST77XX_BLACK);
    tft.fillCircle(64, 60, 6, ST77XX_WHITE);
    tft.setTextColor(ST77XX_CYAN);
    tft.setTextSize(1);
    tft.setCursor(16, 100);
    tft.print("AWAITING COMMAND");
    prevRad = -1;
  }
  int rad = 18 + (int)((sin(frame * .15) + 1) * 7);
  if (rad != prevRad && prevRad > 0) {
    tft.drawCircle(64, 60, prevRad + 10, ST77XX_BLACK);
    tft.drawCircle(64, 60, prevRad, ST77XX_BLACK);
  }
  if (rad != prevRad) {
    tft.drawCircle(64, 60, rad + 10, ST77XX_BLUE);
    tft.drawCircle(64, 60, rad, ST77XX_CYAN);
    prevRad = rad;
  }
}
void drawListening(bool init) {
  if (init) {
    tft.fillScreen(ST77XX_BLACK);
    for (int i=0; i<7; i++) prevH[i] = -1;
  }
  for (int i = 0; i < 7; i++) {
    int h = 12 + (int)(abs(sin(frame * .35 + i * .8)) * 45);
    int x = 16 + i * 16;
    if (h != prevH[i] && prevH[i] > 0) {
      tft.drawFastVLine(x, 60 - prevH[i] / 2, prevH[i], ST77XX_BLACK);
    }
    if (h != prevH[i]) {
      tft.drawFastVLine(x, 60 - h / 2, h, ST77XX_CYAN);
      prevH[i] = h;
    }
  }
}
void drawThinking(bool init) {
  if (init) {
    tft.fillScreen(ST77XX_BLACK);
  }
  // Clear just the animation box instead of whole screen
  tft.fillRect(30, 26, 68, 68, ST77XX_BLACK);
  
  float a = frame * .18;
  tft.drawCircle(64, 60, 30, ST77XX_YELLOW);
  tft.drawCircle(64, 60, 20, ST77XX_ORANGE);
  for (int i = 0; i < 3; i++) {
    float q = a + i * 2.094;
    tft.fillCircle(64 + (int)(cos(q) * 30), 60 + (int)(sin(q) * 30), 4, ST77XX_YELLOW);
  }
  tft.fillCircle(64, 60, 6, ST77XX_WHITE);
}
void drawDog(bool init) {
  if (init) tft.fillScreen(ST77XX_BLACK);
  int phase = dogFrame % 4;
  int x = 15 + phase * 24;
  // Clear only the bounding box of the dog's movement area
  tft.fillRect(0, 40, 128, 60, ST77XX_BLACK);
  
  tft.drawFastHLine(0, 103, 128, ST77XX_GREEN);
  uint16_t c = ST77XX_YELLOW;
  tft.fillRect(x + 10, 58, 24, 18, c);
  tft.fillRect(x + 30, 48, 15, 18, c);
  tft.fillRect(x + 39, 43, 6, 10, c);
  tft.fillRect(x + 42, 55, 3, 3, ST77XX_BLACK);
  tft.fillRect(x + 4, 55, 8, 5, c);
  int leg1 = (phase == 0 || phase == 2) ? 74 : 68;
  int leg2 = (phase == 1 || phase == 3) ? 74 : 68;
  tft.fillRect(x + 14, leg1, 5, 20, c);
  tft.fillRect(x + 28, leg2, 5, 20, c);
}
void drawFace(String mood, bool init) {
  if (!init) return; // Static faces don't need redrawing unless state changes!
  tft.fillScreen(ST77XX_BLACK);
  // header("EMO // FACE", ST77XX_MAGENTA);
  int leftEyeX = 34, rightEyeX = 94, eyeY = 64;
  if (mood == "face_idle") {
    tft.fillRect(leftEyeX - 10, eyeY - 12, 20, 24, ST77XX_CYAN);
    tft.fillRect(rightEyeX - 10, eyeY - 12, 20, 24, ST77XX_CYAN);
  } else if (mood == "face_happy") {
    tft.fillCircle(leftEyeX, eyeY, 12, ST77XX_GREEN);
    tft.fillCircle(rightEyeX, eyeY, 12, ST77XX_GREEN);
    tft.fillRect(leftEyeX-12, eyeY, 24, 13, ST77XX_BLACK);
    tft.fillRect(rightEyeX-12, eyeY, 24, 13, ST77XX_BLACK);
  } else if (mood == "face_sad") {
    tft.fillCircle(leftEyeX, eyeY, 12, ST77XX_BLUE);
    tft.fillCircle(rightEyeX, eyeY, 12, ST77XX_BLUE);
    tft.fillRect(leftEyeX-12, eyeY-12, 24, 13, ST77XX_BLACK);
    tft.fillRect(rightEyeX-12, eyeY-12, 24, 13, ST77XX_BLACK);
  } else if (mood == "face_angry") {
    tft.fillScreen(ST77XX_RED);
    tft.fillTriangle(leftEyeX-10, eyeY-5, leftEyeX+10, eyeY+5, leftEyeX-10, eyeY+15, ST77XX_WHITE);
    tft.fillTriangle(rightEyeX+10, eyeY-5, rightEyeX-10, eyeY+5, rightEyeX+10, eyeY+15, ST77XX_WHITE);
  }
}
void updateTFT() {
  unsigned long now = millis();
  
  bool screenChanged = (screenMode != prevScreenMode);
  prevScreenMode = screenMode;
  
  bool stateChanged = (state != prevState);
  prevState = state;
  
  if (screenMode == "dog_running") {
    if (now - screenTick > 140) {
      screenTick = now; dogFrame++; drawDog(screenChanged);
    }
    return;
  }
  if (screenMode.startsWith("face_")) {
    if (screenChanged) {
      drawFace(screenMode, true);
    }
    return;
  }
  
  if (now - animTick < 70) return;
  animTick = now; frame++;
  
  if (state == IDLE) drawIdle(stateChanged || screenChanged);
  else if (state == LISTENING) drawListening(stateChanged || screenChanged);
  else if (state == THINKING) drawThinking(stateChanged || screenChanged);
  else if (state == ERROR_STATE) {
    if (stateChanged || screenChanged) {
      tft.fillScreen(ST77XX_BLACK);
      header("EMO // ERROR", ST77XX_RED);
      tft.setTextColor(ST77XX_RED);
      tft.setTextSize(2);
      tft.setCursor(25, 55);
      tft.print("ERROR");
    }
  }
}

void applyPixel(int r, int g, int b, int brightness) {
  r = constrain(r, 0, 255); g = constrain(g, 0, 255); b = constrain(b, 0, 255); brightness = constrain(brightness, 0, 255);
  rgb.setBrightness(brightness); rgb.setPixelColor(0, rgb.Color(r, g, b)); rgb.show();
}
void setLight(int r, int g, int b, String mode, int speed, int brightness) {
  lightR = constrain(r, 0, 255); lightG = constrain(g, 0, 255); lightB = constrain(b, 0, 255); lightBrightness = constrain(brightness, 0, 255);
  lightMode = mode; lightSpeed = constrain(speed, 30, 2000); blinkOn = true; breatheValue = 0; breatheDir = 1;
  if (lightMode == "off") applyPixel(0, 0, 0, 0); else applyPixel(lightR, lightG, lightB, lightBrightness);
}
void updateLight() {
  unsigned long now = millis();
  if (lightMode == "solid" || lightMode == "off") return;
  if (lightMode == "blink") {
    if (now - lightTick >= (unsigned long)lightSpeed) {
      lightTick = now; blinkOn = !blinkOn;
      if (blinkOn) applyPixel(lightR, lightG, lightB, lightBrightness); else applyPixel(0, 0, 0, 0);
    }
  } else if (lightMode == "breathe") {
    int step = max(1, 12 - lightSpeed / 180);
    if (now - lightTick >= 20) {
      lightTick = now; breatheValue += breatheDir * step;
      if (breatheValue >= 255) { breatheValue = 255; breatheDir = -1; }
      if (breatheValue <= 0) { breatheValue = 0; breatheDir = 1; }
      int br = (lightBrightness * breatheValue) / 255;
      applyPixel(lightR, lightG, lightB, br);
    }
  }
}

// --- TOOLS ---
String fetchJoke() {
  HTTPClient http;
  http.begin("https://official-joke-api.appspot.com/random_joke");
  int code = http.GET();
  String res = "Joke API failed.";
  if (code == 200) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, http.getString());
    res = doc["setup"].as<String>() + " ... " + doc["punchline"].as<String>();
  }
  http.end();
  return res;
}
String fetchWeather(String city) {
  if (city == "") city = "New York"; // Default
  HTTPClient http;
  http.begin("https://geocoding-api.open-meteo.com/v1/search?name=" + city + "&count=1");
  int code = http.GET();
  if (code != 200) { http.end(); return "Could not find weather data."; }
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, http.getString());
  http.end();
  
  if (doc["results"].isNull()) return "City not found.";
  float lat = doc["results"][0]["latitude"];
  float lon = doc["results"][0]["longitude"];
  
  http.begin("https://api.open-meteo.com/v1/forecast?latitude=" + String(lat) + "&longitude=" + String(lon) + "&current_weather=true");
  code = http.GET();
  String res = "Weather API failed.";
  if (code == 200) {
    DynamicJsonDocument wdoc(1024);
    deserializeJson(wdoc, http.getString());
    float temp = wdoc["current_weather"]["temperature"];
    res = "Weather in " + city + ": " + String(temp) + "C.";
  }
  http.end();
  return res;
}

// --- AI TASK ---
String callOllama(String text, String sysPrompt) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  http.begin(client, aiServerUrl);
  http.setTimeout(30000); // Wait up to 30 seconds for AI response
  http.addHeader("Content-Type", "application/json");
  if (String(apiKey).length() > 0) {
    http.addHeader("Authorization", "Bearer " + String(apiKey));
  }
  
  // Build JSON body
  DynamicJsonDocument reqDoc(8192);
  reqDoc["model"] = aiModel;
  reqDoc["format"] = "json";
  reqDoc["stream"] = false;
  
  // Options
  JsonObject options = reqDoc.createNestedObject("options");
  options["temperature"] = 0.2;
  
  JsonArray msgs = reqDoc.createNestedArray("messages");
  
  JsonObject sysMsg = msgs.createNestedObject();
  sysMsg["role"] = "system";
  sysMsg["content"] = sysPrompt;
  
  for (int i=0; i<chatCount; i+=2) {
    JsonObject um = msgs.createNestedObject();
    um["role"] = "user"; um["content"] = chatHistory[i];
    if (i+1 < chatCount) {
      JsonObject am = msgs.createNestedObject();
      am["role"] = "assistant"; am["content"] = chatHistory[i+1];
    }
  }
  
  JsonObject curMsg = msgs.createNestedObject();
  curMsg["role"] = "user";
  curMsg["content"] = text;
  
  String body;
  serializeJson(reqDoc, body);
  
  int code = http.POST(body);
  String resStr = "";
  if (code == 200) {
    String payload = http.getString();
    Serial.println("API Payload:"); Serial.println(payload);
    
    DynamicJsonDocument resDoc(8192);
    DeserializationError err = deserializeJson(resDoc, payload);
    if (!err) {
      resStr = resDoc["message"]["content"].as<String>();
    } else {
      resStr = "{\"reply\":\"API JSON Parse Error: " + String(err.c_str()) + "\",\"action\":\"face_sad\"}";
    }
  } else {
    String errBody = http.getString();
    Serial.print("Ollama Error: "); Serial.println(code);
    Serial.println(errBody);
    
    // Construct a fallback JSON so the app doesn't crash, and the user sees the error
    String safeError = errBody;
    safeError.replace("\"", "'");
    safeError.replace("\n", " ");
    resStr = "{\"reply\":\"HTTP Error " + String(code) + ": " + safeError + "\",\"action\":\"face_sad\"}";
  }
  http.end();
  return resStr;
}

String cleanJson(String raw) {
  int start = raw.indexOf('{');
  int end = raw.lastIndexOf('}');
  if (start != -1 && end != -1) return raw.substring(start, end + 1);
  return raw;
}

void aiTask(void* p) {
  String text = pendingCommand;
  
  String rawAi = callOllama(text, SYSTEM_PROMPT);
  
  bool success = false;
  DynamicJsonDocument aiDoc(2048);
  if (rawAi != "") {
    String clean = cleanJson(rawAi);
    DeserializationError err = deserializeJson(aiDoc, clean);
    if (!err) {
      String tool = aiDoc["tool"] | "none";
      if (tool != "none") {
        String args = aiDoc["tool_args"] | "";
        String toolRes = "";
        if (tool == "weather") toolRes = fetchWeather(args);
        else if (tool == "joke") toolRes = fetchJoke();
        else toolRes = "Unknown tool.";
        
        // Loop back
        chatHistory[chatCount++] = text;
        chatHistory[chatCount++] = clean;
        if (chatCount >= 4) { // Shift history
          for (int i=0; i<2; i++) chatHistory[i] = chatHistory[i+2];
          chatCount -= 2;
        }
        
        rawAi = callOllama("TOOL RESULT: " + toolRes + ". Now provide final answer with tool='none'.", SYSTEM_PROMPT);
        clean = cleanJson(rawAi);
        err = deserializeJson(aiDoc, clean);
      }
      if (!err) {
        success = true;
        aiAction = aiDoc["action"] | "none";
        aiMode = aiDoc["mode"] | "solid";
        aiScreen = aiDoc["screen"] | "none";
        aiR = aiDoc["r"] | 0; aiG = aiDoc["g"] | 0; aiB = aiDoc["b"] | 0;
        aiSpeed = aiDoc["speed_ms"] | 120;
        aiBrightness = aiDoc["brightness"] | 255;
        aiReply = aiDoc["reply"].as<String>();
        
        chatHistory[chatCount++] = text;
        chatHistory[chatCount++] = clean;
        if (chatCount >= 4) { // Shift history
          for (int i=0; i<2; i++) chatHistory[i] = chatHistory[i+2];
          chatCount -= 2;
        }
      } else {
        Serial.print("Model JSON Parse Error: "); Serial.println(err.c_str());
        aiSuccess = false;
        aiReply = "Model Parse Error: " + String(err.c_str());
      }
    } else {
      Serial.print("Model JSON Parse Error: "); Serial.println(err.c_str());
      aiSuccess = false;
      aiReply = "Model Parse Error: " + String(err.c_str());
    }
  }
  
  if (!success && aiReply == "") {
    aiSuccess = false;
    aiReply = "AI connection or parsing failed.";
  } else if (!success) {
    aiSuccess = false;
  } else {
    aiSuccess = true;
  }
  
  aiBusy = false;
  resultReady = true;
  vTaskDelete(NULL);
}

void startJob(String text) {
  if (aiBusy) return;
  pendingCommand = text; aiBusy = true; resultReady = false; state = THINKING;
  xTaskCreatePinnedToCore(aiTask, "EMOAI", 12000, NULL, 1, NULL, 0); // Need more stack for JSON
}

// --- HTTPS SERVER HANDLERS ---
void handleCommand(HTTPRequest * req, HTTPResponse * res) {
  if (aiBusy) {
    res->setStatusCode(429);
    res->setHeader("Content-Type", "application/json");
    res->println("{\"started\":false,\"error\":\"busy\"}");
    return;
  }
  
  auto params = req->getParams();
  std::string textParam;
  if (!params->getQueryParameter("text", textParam)) {
    res->setStatusCode(400);
    res->setHeader("Content-Type", "application/json");
    res->println("{\"started\":false}");
    return;
  }
  
  String text = String(textParam.c_str());
  text.trim();
  if (!text.length()) {
    res->setStatusCode(400);
    res->setHeader("Content-Type", "application/json");
    res->println("{\"started\":false}");
    return;
  }
  state = LISTENING;
  startJob(text);
  res->setStatusCode(200);
  res->setHeader("Content-Type", "application/json");
  res->println("{\"started\":true}");
}

void handleStatus(HTTPRequest * req, HTTPResponse * res) {
  DynamicJsonDocument doc(512);
  doc["busy"] = aiBusy;
  doc["version"] = resultVersion;
  doc["success"] = resultSuccess;
  doc["reply"] = resultReply;
  doc["r"] = lightR; doc["g"] = lightG; doc["b"] = lightB;
  doc["brightness"] = lightBrightness; doc["mode"] = lightMode;
  doc["screen"] = screenMode;
  
  String out;
  serializeJson(doc, out);
  res->setStatusCode(200);
  res->setHeader("Content-Type", "application/json");
  res->println(out.c_str());
}

void handleRoot(HTTPRequest * req, HTTPResponse * res) {
  const char* page = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EMO (SSL)</title><style>
*{box-sizing:border-box}body{margin:0;background:#03070c;color:#e9fbff;font-family:Arial,sans-serif}
body:before{content:"";position:fixed;inset:0;opacity:.16;pointer-events:none;background-image:linear-gradient(#0af2 1px,transparent 1px),linear-gradient(90deg,#0af2 1px,transparent 1px);background-size:28px 28px}
main{position:relative;max-width:700px;margin:auto;padding:24px 16px 45px}.title{text-align:center;color:#00e5ff;letter-spacing:9px;font-size:38px;text-shadow:0 0 25px #00bfff}.sub{text-align:center;font-size:10px;letter-spacing:3px;color:#7396a5;margin:8px 0 20px}
.orb{margin:25px auto;width:120px;height:120px;border:2px solid #00e5ff;border-radius:50%;box-shadow:0 0 30px #00bfff,inset 0 0 30px #00bfff55;animation:p 2.2s infinite}.busy{animation-duration:.65s}@keyframes p{50%{transform:scale(1.09)}}
.status{text-align:center;color:#00ff9d;font-size:12px;letter-spacing:2px;margin:14px}.panel{background:#071019dd;border:1px solid #00e5ff55;border-radius:18px;padding:15px}.row{display:flex;gap:10px}input{flex:1;min-width:0;height:55px;border-radius:12px;border:1px solid #1c728b;background:#02090f;color:white;padding:12px;font-size:16px}.btn{height:54px;border:0;border-radius:12px;color:white;font-weight:bold;cursor:pointer}.mic{width:62px;background:#073243;border:1px solid #00e5ff}.ask{width:100%;margin-top:10px;background:linear-gradient(135deg,#007b9e,#284fd0)}.ask:disabled{opacity:.45}.reply{margin-top:14px;min-height:110px;padding:15px;border-left:3px solid #00e5ff;background:#02070ccc;border-radius:12px;white-space:pre-wrap}.label{font-size:10px;letter-spacing:2px;color:#00e5ff;margin-bottom:8px}.device{display:flex;gap:14px;align-items:center;margin-top:14px}.lamp{width:48px;height:48px;border-radius:50%;border:1px solid white;background:#000;box-shadow:0 0 15px #fff3}.hint{font-size:11px;color:#7b9ba8;margin-top:15px;line-height:1.5}
</style></head><body><main>
<div class="title">EMO</div><div class="sub">ESP32-S3 / DIRECT AI / SSL SECURE</div>
<div id="orb" class="orb"></div><div id="status" class="status">SYSTEM READY</div>
<div class="panel"><div class="row"><input id="cmd" placeholder="Speak or type anything..."><button class="btn mic" id="mic">MIC</button></div>
<button class="btn ask" id="ask">ASK EMO</button>
<div class="reply" id="reply"><div class="label">EMO</div>Awaiting command.</div>
<div class="device"><div id="lamp" class="lamp"></div><div><b>RGB LIGHT</b><div id="rgb">RGB(0,0,0)</div><div id="mode">solid</div></div></div>
<div class="hint">Microphone requires HTTPS connection. Accept the self-signed certificate warning to proceed.</div>
</div></main>
<script>
const $=id=>document.getElementById(id), cmd=$('cmd'), ask=$('ask'), mic=$('mic'), orb=$('orb'), status=$('status'), reply=$('reply');
let lastVersion=-1, recognition=null, polling=false;
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML}
function setBusy(v){ask.disabled=v;orb.classList.toggle('busy',v);status.textContent=v?'THINKING':'SYSTEM READY'}
async function send(){
 window.speechSynthesis.speak(new SpeechSynthesisUtterance('')); // Pre-warm TTS engine to prevent mobile blocking
 const text=cmd.value.trim(); if(!text||polling)return;
 cmd.value=''; polling=true; setBusy(true); reply.innerHTML='<div class="label">PROCESSING</div>EMO IS ANALYZING YOUR COMMAND...';
 try{let r=await fetch('/command?text='+encodeURIComponent(text));let d=await r.json();if(!d.started)throw Error(d.error||'start failed'); poll();}
 catch(e){polling=false;setBusy(false);reply.innerHTML='<div class="label">ERROR</div>'+esc(e.message)}
}
async function poll(){
 try{
   const r=await fetch('/status',{cache:'no-store'}); const d=await r.json(); update(d);
   if(d.busy){setTimeout(poll,500);return;}
   if(d.version!==lastVersion){lastVersion=d.version;polling=false;setBusy(false);reply.innerHTML='<div class="label">'+ (d.success?'EMO':'ERROR')+'</div>'+esc(d.reply||'No response');if(d.success&&d.reply)speak(d.reply);return;}
   setTimeout(poll,500);
 }catch(e){polling=false;setBusy(false);reply.innerHTML='<div class="label">CONNECTION ERROR</div>'+esc(e.message)}
}
function update(d){$('lamp').style.background=`rgb(${d.r},${d.g},${d.b})`;$('lamp').style.boxShadow=`0 0 28px rgb(${d.r},${d.g},${d.b})`;$('rgb').textContent=`RGB(${d.r},${d.g},${d.b}) / ${d.brightness}`;$('mode').textContent=d.mode+(d.screen!=='none'?' / '+d.screen:'')}
ask.onclick=send;cmd.onkeydown=e=>{if(e.key==='Enter')send()};
function speak(text){
 window.speechSynthesis.cancel(); 
 let u = new SpeechSynthesisUtterance(text);
 u.rate = 0.9; u.pitch = 1.1; 
 window.speechSynthesis.speak(u);
}
async function voice(){
 window.speechSynthesis.speak(new SpeechSynthesisUtterance('')); // Pre-warm TTS engine
 const SR=window.SpeechRecognition||window.webkitSpeechRecognition;
 if(!SR){reply.innerHTML='<div class="label">VOICE</div>Speech recognition is not supported by this browser.';return;}
 try{await navigator.mediaDevices.getUserMedia({audio:true});}catch(e){reply.innerHTML='<div class="label">MICROPHONE BLOCKED</div>'+esc(e.name+': '+e.message)+'<br>Open browser site permissions and allow Microphone. Voice recognition requires HTTPS.';return;}
 recognition=new SR(); recognition.lang='en-US'; recognition.interimResults=true; recognition.continuous=false;
 mic.textContent='STOP';status.textContent='LISTENING';
 recognition.onresult=e=>{
   let t='';
   for(let i=0;i<e.results.length;i++) t+=e.results[i][0].transcript;
   cmd.value=t;
 };
 recognition.onend=()=>{mic.textContent='MIC';recognition=null;if(cmd.value.trim())send();else status.textContent='SYSTEM READY'};
 recognition.onerror=e=>{
   mic.textContent='MIC';recognition=null;status.textContent='SYSTEM READY';
   let errMsg = e.error;
   if(errMsg === 'network') errMsg += " (Your phone MUST have internet access for Web Speech API to work!)";
   reply.innerHTML='<div class="label">VOICE ERROR</div>'+esc(errMsg);
 };
 recognition.start();
}
mic.onclick=()=>{if(recognition)recognition.stop();else voice()};
(async()=>{try{let d=await (await fetch('/status')).json();lastVersion=d.version;update(d)}catch(e){}})();
</script></body></html>
)HTML"; // "

  res->setStatusCode(200);
  res->setHeader("Content-Type", "text/html");
  
  // Send HTML in chunks to prevent TLS buffer overflow and ESP32 crashes
  int len = strlen(page);
  int offset = 0;
  while (offset < len) {
    int chunkSize = min(1024, len - offset);
    res->write((uint8_t*)(page + offset), chunkSize);
    offset += chunkSize;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n--- ESP32 BOOTING ---");
  
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
  Serial.println("Backlight ON");
  
  SPI.begin(TFT_CLK, -1, TFT_MOSI, TFT_CS);
  Serial.println("SPI Started");
  
  tft.initR(INITR_144GREENTAB); 
  tft.setRotation(0); 
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("TFT Initialized");
  rgb.begin(); rgb.show(); setLight(0, 0, 0, "off", 120, 0);

  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1); tft.setCursor(5, 5);
  tft.println("Connecting WiFi...");

  WiFi.mode(WIFI_AP_STA); 
  WiFi.setHostname("emo"); 
  WiFi.softAP("EMO_NETWORK", "checking1234");
  
  // Try primary WiFi
  WiFi.begin(homeSSID, homePassword);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(300);
  
  // Try secondary WiFi if primary fails
  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(ST77XX_BLACK); tft.setCursor(5, 5);
    tft.println("Primary WiFi Failed.");
    tft.println("Trying Backup WiFi...");
    WiFi.begin("Helping Poors", "checking1234");
    start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(300);
  }
  
  
  tft.fillScreen(ST77XX_BLACK); tft.setCursor(5, 5);
  if (WiFi.status() == WL_CONNECTED) {
    tft.println("WiFi Connected!"); tft.print("IP: "); tft.println(WiFi.localIP());
  } else { 
    tft.println("WiFi Failed!"); 
    tft.print("AP IP: "); tft.println(WiFi.softAPIP());
  }
  if (MDNS.begin("emo")) Serial.println("MDNS started: https://emo.local");

  // Generate SSL Certificate
  Serial.println("Generating SSL Cert (takes a moment)...");
  tft.println("Gen SSL Cert...");
  cert = new SSLCert();
  createSelfSignedCert(*cert, KEYSIZE_2048, "CN=emo.local,O=EMO,C=US");
  secureServer = new HTTPSServer(cert);

  ResourceNode * nodeRoot = new ResourceNode("/", "GET", &handleRoot);
  ResourceNode * nodeCmd = new ResourceNode("/command", "GET", &handleCommand);
  ResourceNode * nodeStatus = new ResourceNode("/status", "GET", &handleStatus);
  
  secureServer->registerNode(nodeRoot);
  secureServer->registerNode(nodeCmd);
  secureServer->registerNode(nodeStatus);
  secureServer->start();

  Serial.println("HTTPS Server started.");
  tft.println("HTTPS Started!");
  delay(2000);
}

void loop() {
  secureServer->loop();
  updateLight();

  if (resultReady) {
    resultReady = false;
    resultSuccess = aiSuccess; resultReply = aiReply;
    if (aiSuccess) {
      if (aiAction == "set_light") setLight(aiR, aiG, aiB, aiMode, aiSpeed, aiBrightness);
      if (aiScreen == "dog_running") screenMode = "dog_running";
      else if (aiScreen.startsWith("face_")) { screenMode = aiScreen; screenTick = 0; }
      else if (aiScreen == "clear") { screenMode = "none"; state = IDLE; } 
      else if (aiScreen == "none" && screenMode != "dog_running" && !screenMode.startsWith("face_")) state = IDLE;
      else if (aiScreen == "none") { state = IDLE; }
    } else state = ERROR_STATE;
    resultVersion++;
  }
  updateTFT();
}
