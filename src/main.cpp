#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <NeoPixelBus.h>
#include <NeoPixelAnimator.h>

// ---- Matrix config ----------------------------------------------------------

#define LED_PIN        4       // GPIO pin for LED data
#define MATRIX_WIDTH   8
#define MATRIX_HEIGHT  8
#define LED_COUNT      (MATRIX_WIDTH * MATRIX_HEIGHT)  // 64

// ESP32-C3: use RMT channel 0 for reliable 800 kHz signal
NeoPixelBus<NeoGrbFeature, NeoEsp32Rmt0Ws2812xMethod> strip(LED_COUNT, LED_PIN);

// One animator channel per pixel — allows independent per-pixel animations
NeoPixelAnimator animator(LED_COUNT);

// ---- Must match the struct in the sender ------------------------------------

struct KnockMessage {
  uint32_t timestamp_ms;
  float    peak;
};

// ---- Animation helpers ------------------------------------------------------

// Simple linear interpolation between two colors over an animation's progress
struct AnimState {
  RgbColor startColor;
  RgbColor endColor;
};

static AnimState animStates[LED_COUNT];

// Callback fired by the animator each frame for pixel `param.index`
static void fadePixel(const AnimationParam& param) {
  RgbColor color = RgbColor::LinearBlend(
    animStates[param.index].startColor,
    animStates[param.index].endColor,
    param.progress
  );
  strip.SetPixelColor(param.index, color);
}

// Queue a random color fade for one pixel
static void startPixelFade(uint16_t pixel) {
  animStates[pixel].startColor = strip.GetPixelColor<RgbColor>(pixel);

  // Random dim hue for the idle shimmer
  uint8_t r = random(0, 30);
  uint8_t g = random(0, 30);
  uint8_t b = random(0, 30);
  animStates[pixel].endColor = RgbColor(r, g, b);

  uint16_t duration = random(400, 1200);  // ms
  animator.StartAnimation(pixel, duration, fadePixel);
}

// Flash all pixels a bright color then fade back to off (knock reaction)
static void triggerKnockFlash(float peak) {
  // Scale brightness by peak (0–1023 assumed range, clamp to 255)
  uint8_t brightness = (uint8_t)constrain((int)(peak / 4.0f), 0, 255);
  RgbColor flashColor(brightness, brightness / 4, 0);  // warm amber

  for (uint16_t i = 0; i < LED_COUNT; i++) {
    animStates[i].startColor = flashColor;
    animStates[i].endColor   = RgbColor(0);
    animator.StartAnimation(i, 600, fadePixel);
  }
}

// ---- Knock handler ----------------------------------------------------------

static void onKnock(const KnockMessage &msg) {
  Serial.printf("Knock received: sender_t=%lu ms  peak=%.0f\n",
                msg.timestamp_ms, msg.peak);
  triggerKnockFlash(msg.peak);
}

// ---- ESP-NOW plumbing -------------------------------------------------------

static void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(KnockMessage)) {
    Serial.printf("Unexpected payload size: %d bytes\n", len);
    return;
  }
  KnockMessage msg;
  memcpy(&msg, data, sizeof(msg));
  onKnock(msg);
}

// ---- Arduino lifecycle ------------------------------------------------------

void setup() {
  Serial.begin(115200);

  strip.Begin();
  strip.Show();  // clear all pixels

  // Seed idle shimmer — stagger start times so pixels don't all sync up
  for (uint16_t i = 0; i < LED_COUNT; i++) {
    animStates[i].startColor = RgbColor(0);
    animStates[i].endColor   = RgbColor(0);
    uint16_t delay = random(0, 2000);
    animator.StartAnimation(i, delay, [](const AnimationParam& param) {
      // empty first pass just to stagger; real fade starts in loop
    });
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onReceive);
  Serial.println("ESP-NOW receiver ready");
}

void loop() {
  if (animator.IsAnimating()) {
    animator.UpdateAnimations();
    strip.Show();
  }

  // Restart idle shimmer for any pixel whose animation just finished
  for (uint16_t i = 0; i < LED_COUNT; i++) {
    if (!animator.IsAnimationActive(i)) {
      startPixelFade(i);
    }
  }

 
}
