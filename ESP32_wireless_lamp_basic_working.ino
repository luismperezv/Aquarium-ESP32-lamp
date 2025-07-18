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

int brightness = 128;
const int step = 26;
const int flashThreshold = 26;
bool led1Enabled = true;
bool led2Enabled = true;

const unsigned long longPressDuration = 2000;
const unsigned long debounceDelay = 30;
const int flashDelay = 100;

enum ButtonState { IDLE, PRESSED, HELD };

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

void setup() {
  delay(1000);
  Serial.begin(115200);

  pinMode(buttonUp, INPUT_PULLUP);
  pinMode(buttonDown, INPUT_PULLUP);

  ledcAttach(ledPin1, 5000, 8);
  ledcAttach(ledPin2, 5000, 8);
  updateLEDs();

  // Connect to Wi-Fi
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

  // Web routes
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
  handleButton(upBtn, led1Enabled, true);
  handleButton(downBtn, led2Enabled, false);
  server.handleClient();
}

void handleButton(Button &btn, bool &ledEnabled, bool isIncrease) {
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
            adjustBrightness(isIncrease);
          }
          btn.state = IDLE;
        } else if (now - btn.pressStart >= longPressDuration) {
          ledEnabled = !ledEnabled;
          updateLEDs();
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
  int oldBrightness = brightness;

  if (isIncrease) {
    brightness = min(255, brightness + step);
  } else {
    brightness = max(0, brightness - step);
  }

  updateLEDs();

  if ((brightness == 255 && oldBrightness < 255) ||
      (brightness == flashThreshold && oldBrightness > flashThreshold)) {
    flashLEDs();
  }
}

void updateLEDs() {
  ledcWrite(ledPin1, led1Enabled ? brightness : 0);
  ledcWrite(ledPin2, led2Enabled ? brightness : 0);
}

void flashLEDs() {
  ledcWrite(ledPin1, 0);
  ledcWrite(ledPin2, 0);
  delay(flashDelay);
  updateLEDs();
}

void handleRoot() {
  String html = "<h1>LED Lamp Control</h1>";
  html += "<p>Brightness: " + String(brightness) + " / 255</p>";
  html += "<p>LED1: " + String(led1Enabled ? "ON" : "OFF") + "</p>";
  html += "<p>LED2: " + String(led2Enabled ? "ON" : "OFF") + "</p>";
  html += "<form action='/up' method='POST'><button>+</button></form>";
  html += "<form action='/down' method='POST'><button>-</button></form>";
  html += "<form action='/toggle1' method='POST'><button>Toggle LED1</button></form>";
  html += "<form action='/toggle2' method='POST'><button>Toggle LED2</button></form>";
  server.send(200, "text/html", html);
}
