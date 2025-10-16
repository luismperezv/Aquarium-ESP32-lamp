#include <WiFi.h>
#include <WebServer.h>
#include <time.h>  // time for Natural Light Cycle (NLC)

const char* ssid = "FHCPE-c9S4";
const char* password = "UpmcXfh6";

// Static IP configuration
IPAddress local_IP(192, 168, 8, 210);
IPAddress gateway(192, 168, 8, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(1, 1, 1, 1);
IPAddress secondaryDNS(8, 8, 8, 8);

WebServer server(80);

// NOTE: Keeping your existing pins as requested
const int ledPin1 = 3;
const int ledPin2 = 4;
const int buttonUp = 7;
const int buttonDown = 0;
const int wifiStatusLed = 8;

int brightness1 = 128;
int brightness2 = 128;
const int step = 26;
const int flashThreshold = 26;
bool led1Enabled = true;
bool led2Enabled = true;

const unsigned long longPressDuration = 2000;
const unsigned long debounceDelay = 30;
const int flashDelay = 100;

enum ButtonState { IDLE, PRESSED, HELD };
enum Mode { NORMAL, TUNE1, TUNE2 };

struct Button {
  int pin;
  unsigned long pressStart = 0;
  unsigned long lastStateChange = 0;
  bool lastReading = true;
  ButtonState state = IDLE;
  bool handled = false;
};

Button upBtn = {buttonUp};
Button downBtn = {buttonDown};

Mode mode = NORMAL;
unsigned long lastInteraction = 0;
const unsigned long modeTimeout = 90000;

unsigned long lastWifiBlink = 0;
bool wifiLedState = false;

// ===================== Natural Light Cycle (NLC) =====================
bool naturalCycleEnabled = true;      // ON by default at boot
unsigned long lastUserOverride = 0;   // millis of last manual interaction (0 = none yet)
int overrideMinutes = 120;            // configurable in UI; default 120 minutes

// Slew control for smooth transitions
unsigned long lastSlew = 0;
const unsigned long slewIntervalMs = 200;  // update every 200ms
const int slewStep = 3;                    // max PWM change per tick

// On first valid time after boot, jump instantly to target once
bool nlcInitialApplied = false;

inline unsigned long overrideWindowMs() {
  // clamp to 1..480 minutes for sanity
  int m = constrain(overrideMinutes, 1, 480);
  return (unsigned long)m * 60UL * 1000UL;
}

inline void markUserOverride() { lastUserOverride = millis(); }

int localMinutes() {
  struct tm t;
  if (!getLocalTime(&t, 10)) return -1;
  return t.tm_hour * 60 + t.tm_min;
}

// 08:00..23:00 sinusoidal hump (0 outside window)
int naturalTargetBrightness() {
  int mins = localMinutes();
  if (mins < 0) return -1;
  const int startM = 8 * 60;   // 08:00
  const int endM   = 23 * 60;  // 23:00
  if (mins < startM || mins >= endM) return 0;

  float t = float(mins - startM) / float(endM - startM); // 0..1
  float b = sinf(t * 3.1415926f); // smooth 0..1..0
  float g = powf(fmaxf(0.f, b), 1.2f); // gentle gamma
  int target = (int)roundf(g * 255.0f);
  if (target > 240) target = 240; // keep under max for softness
  return target;
}

void serviceNaturalCycle() {
  if (!naturalCycleEnabled) return;

  // If we've had a manual change recently, respect the override window
  if (lastUserOverride != 0 && (millis() - lastUserOverride < overrideWindowMs())) return;

  int target = naturalTargetBrightness();
  if (target < 0) return; // time not ready yet

  // First time after time becomes valid: jump instantly to target (per your request)
  if (!nlcInitialApplied) {
    brightness1 = target;
    brightness2 = target;
    updateLEDs();
    nlcInitialApplied = true;
    return;
  }

  // Afterwards, slew gently toward the target
  unsigned long now = millis();
  if (now - lastSlew < slewIntervalMs) return;
  lastSlew = now;

  auto slewOne = [&](int &b) {
    if (b < target)      b = min(target, b + slewStep);
    else if (b > target) b = max(target, b - slewStep);
  };

  int before1 = brightness1, before2 = brightness2;
  slewOne(brightness1);
  slewOne(brightness2);

  if (brightness1 != before1 || brightness2 != before2) {
    updateLEDs();
  }
}
// ====================================================================

void setup() {
  delay(1000);
  Serial.begin(115200);

  pinMode(buttonUp, INPUT_PULLUP);
  pinMode(buttonDown, INPUT_PULLUP);
  pinMode(wifiStatusLed, OUTPUT);

  // Keep your original LEDC calls as-is
  ledcAttach(ledPin1, 5000, 8);
  ledcAttach(ledPin2, 5000, 8);
  updateLEDs();  // starts at 128/128

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("⚠️ Failed to configure static IP");
  }
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    updateWifiLed();
    delay(10);
    yield();
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());
  digitalWrite(wifiStatusLed, LOW);  // Solid ON (active-low fix)

  // Time setup for London (GMT/BST)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1); // London time rules
  tzset();
  // Try to fetch time quickly (best-effort, non-fatal)
  struct tm tmnow;
  for (int i = 0; i < 20 && !getLocalTime(&tmnow, 50); i++) { delay(50); }

  server.on("/", handleRoot);
  server.on("/up", []() {
    adjustBrightness(true);
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/down", []() {
    adjustBrightness(false);
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/toggle1", []() {
    led1Enabled = !led1Enabled;
    markUserOverride();
    updateLEDs();
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/toggle2", []() {
    led2Enabled = !led2Enabled;
    markUserOverride();
    updateLEDs();
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/state", HTTP_GET, handleState);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/favicon.ico", HTTP_GET, [](){ server.send(204); });
  server.on("/cmd", HTTP_POST, handleCmd);

  server.begin();
  Serial.println("Web server started");
}

void loop() {
  handleButton(upBtn, true);
  handleButton(downBtn, false);
  checkFineTuneTimeout();
  updateWifiLed();
  serviceNaturalCycle(); // drive schedule
  server.handleClient();
}

void handleButton(Button &btn, bool isUp) {
  unsigned long now = millis();
  bool reading = digitalRead(btn.pin);

  if (reading != btn.lastReading) {
    btn.lastStateChange = now;
  }

  if ((now - btn.lastStateChange) > debounceDelay) {
    switch (btn.state) {
      case IDLE:
        if (reading == LOW) {
          btn.pressStart = now;
          btn.state = PRESSED;
          btn.handled = false;
        }
        break;

      case PRESSED:
        if (reading == HIGH) {
          if (!btn.handled) {
            lastInteraction = now;
            if (mode == NORMAL) {
              adjustBrightness(isUp);
            } else if (mode == TUNE1 && isUp) {
              adjustChannelBrightness(1, true);
            } else if (mode == TUNE1 && !isUp) {
              adjustChannelBrightness(1, false);
            } else if (mode == TUNE2 && isUp) {
              adjustChannelBrightness(2, true);
            } else if (mode == TUNE2 && !isUp) {
              adjustChannelBrightness(2, false);
            }
          }
          btn.state = IDLE;
        } else if (now - btn.pressStart >= longPressDuration) {
          lastInteraction = now;
          if (mode == NORMAL) {
            if (isUp) enterFineTuneMode(1);
            else enterFineTuneMode(2);
          } else {
            exitFineTuneMode();
          }
          btn.handled = true;
          btn.state = HELD;
        }
        break;

      case HELD:
        if (reading == HIGH) {
          btn.state = IDLE;
        }
        break;
    }
  }

  btn.lastReading = reading;
}

void adjustBrightness(bool isIncrease) {
  markUserOverride();
  int old1 = brightness1;
  int old2 = brightness2;

  if (isIncrease) {
    brightness1 = min(255, brightness1 + step);
    brightness2 = min(255, brightness2 + step);
  } else {
    brightness1 = max(0, brightness1 - step);
    brightness2 = max(0, brightness2 - step);
  }

  updateLEDs();

  if ((brightness1 == 255 && old1 < 255) ||
      (brightness1 == flashThreshold && old1 > flashThreshold) ||
      (brightness2 == 255 && old2 < 255) ||
      (brightness2 == flashThreshold && old2 > flashThreshold)) {
    flashLEDs();
  }
}

void adjustChannelBrightness(int channel, bool isIncrease) {
  markUserOverride();
  int &b = (channel == 1) ? brightness1 : brightness2;
  int oldBrightness = b;
  b = constrain(b + (isIncrease ? step : -step), 0, 255);
  updateLEDs();

  if ((b == 255 && oldBrightness < 255) ||
      (b == flashThreshold && oldBrightness > flashThreshold)) {
    flashLEDs();
  }
}

void updateLEDs() {
  ledcWrite(ledPin1, led1Enabled ? brightness1 : 0);
  ledcWrite(ledPin2, led2Enabled ? brightness2 : 0);
}

void flashLEDs() {
  int b1 = brightness1;
  int b2 = brightness2;
  ledcWrite(ledPin1, 0);
  ledcWrite(ledPin2, 0);
  delay(flashDelay);
  ledcWrite(ledPin1, b1);
  ledcWrite(ledPin2, b2);
}

void enterFineTuneMode(int channel) {
  mode = (channel == 1) ? TUNE1 : TUNE2;
  ledcWrite(ledPin1, 0);
  ledcWrite(ledPin2, 0);
  delay(200);
  for (int i = 0; i < 2; i++) {
    if (channel == 1) {
      ledcWrite(ledPin1, 128);
      delay(150);
      ledcWrite(ledPin1, 0);
      delay(150);
    } else {
      ledcWrite(ledPin2, 128);
      delay(150);
      ledcWrite(ledPin2, 0);
      delay(150);
    }
  }
  updateLEDs();
  lastInteraction = millis();
}

void exitFineTuneMode() {
  mode = NORMAL;
  for (int i = 0; i < 2; i++) {
    updateLEDs();
    delay(150);
    ledcWrite(ledPin1, 0);
    ledcWrite(ledPin2, 0);
    delay(150);
  }
  updateLEDs();
}

void checkFineTuneTimeout() {
  if ((mode == TUNE1 || mode == TUNE2) && millis() - lastInteraction > modeTimeout) {
    exitFineTuneMode();
  }
}

void updateWifiLed() {
  unsigned long now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(wifiStatusLed, LOW);  // ON (active-low)
  } else if (now - lastWifiBlink >= 500) {
    wifiLedState = !wifiLedState;
    digitalWrite(wifiStatusLed, wifiLedState ? LOW : HIGH);  // Blink (active-low)
    lastWifiBlink = now;
  }
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>LED Lamp Control</title>
  <style>
    :root { color-scheme: light dark; }
    body { font-family: system-ui, -apple-system, Segoe UI, Roboto, sans-serif; margin: 20px; line-height: 1.3; }
    .card { max-width: 620px; padding: 16px; border-radius: 14px; box-shadow: 0 2px 12px rgba(0,0,0,.1); margin: auto; }
    h1 { margin: 0 0 12px; font-size: 1.4rem; }
    .row { display: grid; grid-template-columns: 140px 1fr auto; gap: 10px; align-items: center; margin: 14px 0; }
    .muted { opacity: .7; font-size: .9rem; }
    input[type=range] { width: 100%; }
    .switch { display:inline-flex; align-items:center; gap:8px; }
    button { padding: 8px 12px; border-radius: 10px; border: 1px solid #aaa4; background: transparent; cursor: pointer; }
    .bar { display:flex; gap:10px; flex-wrap: wrap; margin-top: 12px; }
    .ok { color: #2a8f2a; } .warn { color: #b36b00; }
    .err { color: #c22; }
    .small { font-size:.85rem; }
    .pill { padding:2px 8px; border-radius:999px; border:1px solid #aaa4; }
    input[type=number]{ width: 80px; padding:6px 8px; border-radius:8px; border:1px solid #aaa4; }
    .inline{ display:flex; gap:10px; align-items:center; flex-wrap:wrap; }
  </style>
</head>
<body>
  <div class="card">
    <h1>LED Lamp Control</h1>
    <div class="muted small">
      IP: <span id="ip">…</span> • Wi-Fi: <span id="wifi">…</span>
      • Mode: <span id="mode">NORMAL</span>
    </div>

    <div class="row">
      <div>LED1 Brightness</div>
      <input id="b1" type="range" min="0" max="255" step="1" />
      <div><span id="b1v">0</span>/255</div>
    </div>
    <div class="row">
      <div>LED1</div>
      <div class="switch"><input id="e1" type="checkbox"/><label for="e1">Enabled</label></div>
      <div></div>
    </div>

    <div class="row">
      <div>LED2 Brightness</div>
      <input id="b2" type="range" min="0" max="255" step="1" />
      <div><span id="b2v">0</span>/255</div>
    </div>
    <div class="row">
      <div>LED2</div>
      <div class="switch"><input id="e2" type="checkbox"/><label for="e2">Enabled</label></div>
      <div></div>
    </div>

    <div class="bar">
      <button onclick="cmd('up')">+ (both)</button>
      <button onclick="cmd('down')">- (both)</button>
      <button onclick="cmd('toggle1')">Toggle LED1</button>
      <button onclick="cmd('toggle2')">Toggle LED2</button>
      <button id="nlcbtn" onclick="toggleNLC()">Natural cycle: Off</button>
      <span id="status" class="pill muted">idle</span>
    </div>

    <div class="row">
      <div>Override duration</div>
      <div class="inline">
        <input id="ovr" type="number" min="1" max="480" step="1"/>
        <button onclick="saveOverride()">Save</button>
      </div>
      <div class="muted small">minutes</div>
    </div>

    <p class="muted small">Tip: sliders update on release; toggles are instant. NLC is on by default after boot: lights start at 128 and jump to the correct level once time sync succeeds.</p>
  </div>

<script>
const $ = (id)=>document.getElementById(id);
const statusPill = $("status");
let inflight = 0; let lastState = null;

function setStatus(txt, cls="muted") {
  statusPill.textContent = txt;
  statusPill.className = "pill " + cls;
}

async function fetchState() {
  try {
    const r = await fetch('/state', {cache:'no-store'});
    const j = await r.json();
    $("b1").value = j.b1; $("b1v").textContent = j.b1;
    $("b2").value = j.b2; $("b2v").textContent = j.b2;
    $("e1").checked = j.e1; $("e2").checked = j.e2;
    $("ip").textContent = j.ip || location.host;
    $("wifi").textContent = j.wifi ? "Connected" : "Disconnected";
    $("wifi").className = j.wifi ? "ok" : "warn";
    $("mode").textContent = j.mode || "NORMAL";
    $("nlcbtn").textContent = "Natural cycle: " + (j.nlc ? "On" : "Off");
    $("ovr").value = j.override_min ?? 120;
    lastState = j;
  } catch(e) {
    setStatus("state error", "err");
  }
}

function debounce(fn, ms=150) {
  let t; return (...a)=>{ clearTimeout(t); t=setTimeout(()=>fn(...a), ms); };
}

async function postForm(path, data) {
  inflight++; setStatus("saving…");
  try {
    const body = new URLSearchParams(data);
    const r = await fetch(path, {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body});
    if (!r.ok) throw new Error(r.status);
    await fetchState();
    setStatus("ok", "ok");
  } catch(e) {
    setStatus("error", "err");
  } finally {
    inflight--; if (inflight<=0) setStatus("idle");
  }
}

const sendSet = debounce(()=> {
  postForm('/set', {
    b1: $("b1").value,
    b2: $("b2").value,
    e1: $("e1").checked ? 1 : 0,
    e2: $("e2").checked ? 1 : 0
  });
}, 200);

["b1","b2"].forEach(id=>{
  $(id).addEventListener('change', sendSet);
  $(id).addEventListener('input', ()=>{ $(id+"v").textContent=$(id).value; });
});

["e1","e2"].forEach(id=>{
  $(id).addEventListener('change', ()=> postForm('/set', {[id]: $(id).checked?1:0}));
});

async function cmd(a){ await postForm('/cmd', {a}); }

async function toggleNLC(){
  const turningOn = $("nlcbtn").textContent.includes("Off");
  await postForm('/set', { nlc: turningOn ? 1 : 0 });
}

async function saveOverride(){
  const v = Math.max(1, Math.min(480, parseInt($("ovr").value||"120",10)));
  await postForm('/set', { override_min: v });
}

fetchState();
setInterval(fetchState, 1500);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleState() {
  String s = "{";
  s += "\"b1\":" + String(brightness1) + ",";
  s += "\"b2\":" + String(brightness2) + ",";
  s += "\"e1\":" + String(led1Enabled ? "true":"false") + ",";
  s += "\"e2\":" + String(led2Enabled ? "true":"false") + ",";
  s += "\"wifi\":" + String(WiFi.status() == WL_CONNECTED ? "true":"false") + ",";
  s += "\"nlc\":" + String(naturalCycleEnabled ? "true":"false") + ",";
  s += "\"override_min\":" + String(overrideMinutes) + ",";
  s += "\"mode\":\"" + String(
        mode == NORMAL ? "NORMAL" : (mode == TUNE1 ? "TUNE1" : "TUNE2")) + "\",";
  s += "\"ip\":\"" + (WiFi.isConnected() ? WiFi.localIP().toString() : String("")) + "\"";
  s += "}";
  server.send(200, "application/json", s);
}

// Accepts POST form: b1,b2 (0-255), e1,e2 (0/1), nlc (0/1), override_min (1..480)
void handleSet() {
  auto getArg = [&](const String& name)->String{
    if (server.hasArg(name)) return server.arg(name);
    String v = server.arg(name);
    return v;
  };

  bool changed = false;

  if (getArg("b1").length()) {
    int v = constrain(getArg("b1").toInt(), 0, 255);
    if (v != brightness1) { brightness1 = v; changed = true; }
  }
  if (getArg("b2").length()) {
    int v = constrain(getArg("b2").toInt(), 0, 255);
    if (v != brightness2) { brightness2 = v; changed = true; }
  }
  if (getArg("e1").length()) {
    bool v = getArg("e1").toInt() != 0;
    if (v != led1Enabled) { led1Enabled = v; changed = true; }
  }
  if (getArg("e2").length()) {
    bool v = getArg("e2").toInt() != 0;
    if (v != led2Enabled) { led2Enabled = v; changed = true; }
  }
  if (getArg("nlc").length()) {
    bool v = getArg("nlc").toInt() != 0;
    if (v != naturalCycleEnabled) {
      naturalCycleEnabled = v;
      // If turning NLC on, allow immediate control by schedule unless user acts
      if (naturalCycleEnabled) {
        nlcInitialApplied = false;    // re-apply on next valid time
        // Do not set lastUserOverride here; we want NLC to take over
      }
    }
  }
  if (getArg("override_min").length()) {
    int v = getArg("override_min").toInt();
    v = constrain(v, 1, 480);
    overrideMinutes = v;
    // no brightness change; no markUserOverride()
  }

  if (changed) {
    markUserOverride(); // manual change
    updateLEDs();
  }
  server.send(200, "text/plain", "OK");
}

void handleCmd() {
  String a = server.arg("a");  // expects: up | down | toggle1 | toggle2
  markUserOverride();
  if (a == "up") {
    adjustBrightness(true);
  } else if (a == "down") {
    adjustBrightness(false);
  } else if (a == "toggle1") {
    led1Enabled = !led1Enabled; 
    updateLEDs();
  } else if (a == "toggle2") {
    led2Enabled = !led2Enabled; 
    updateLEDs();
  }
  server.send(200, "text/plain", "OK");
}