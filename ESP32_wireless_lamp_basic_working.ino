#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "FHCPE-c9S4";
const char* password = "UpmcXfh6";

// Static IP configuration
IPAddress local_IP(192, 168, 8, 210);
IPAddress gateway(192, 168, 8, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(1, 1, 1, 1);
IPAddress secondaryDNS(8, 8, 8, 8);

WebServer server(80);

const int ledPin1 = 3;
const int ledPin2 = 4;
const int buttonUp = 7;
const int buttonDown = 0;

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

void setup() {
  delay(1000);
  Serial.begin(115200);

  pinMode(buttonUp, INPUT_PULLUP);
  pinMode(buttonDown, INPUT_PULLUP);

  ledcAttach(ledPin1, 5000, 8);
  ledcAttach(ledPin2, 5000, 8);
  updateLEDs();

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("⚠️ Failed to configure static IP");
  }
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

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
    updateLEDs();
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/toggle2", []() {
    led2Enabled = !led2Enabled;
    updateLEDs();
    server.sendHeader("Location", "/");
    server.send(303);
  });

  server.begin();
  Serial.println("Web server started");
}

void loop() {
  handleButton(upBtn, true);
  handleButton(downBtn, false);
  checkFineTuneTimeout();
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

void handleRoot() {
  String html = "<h1>LED Lamp Control</h1>";
  html += "<p>Brightness1: " + String(brightness1) + " / 255</p>";
  html += "<p>Brightness2: " + String(brightness2) + " / 255</p>";
  html += "<p>LED1: " + String(led1Enabled ? "ON" : "OFF") + "</p>";
  html += "<p>LED2: " + String(led2Enabled ? "ON" : "OFF") + "</p>";
  html += "<form action='/up' method='POST'><button>+</button></form>";
  html += "<form action='/down' method='POST'><button>-</button></form>";
  html += "<form action='/toggle1' method='POST'><button>Toggle LED1</button></form>";
  html += "<form action='/toggle2' method='POST'><button>Toggle LED2</button></form>";
  server.send(200, "text/html", html);
}
