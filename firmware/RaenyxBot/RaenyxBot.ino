/*
 * RaenyxBot - ESP32-S3 voice assistant with a cute face
 * =====================================================
 * What changed vs the Groq version:
 *  - NO API keys on the device. The device talks only to your own server
 *    (server/app.py, hosted on Render), which holds the Gemini key.
 *  - Greeting is in English. Replies follow the user's language (server-side
 *    system prompt), spoken with Gemini TTS (realistic voice).
 *  - Cuter face (sparkle eyes, blush) + amplitude lip sync while speaking:
 *    the mouth follows the loudness of the audio actually being played.
 *  - Song mode: say "play the song blue" / "blue by yung kai" and the device
 *    streams the mp3 from the server, shows the song's JPG as background and
 *    displays the .lrc lyrics in real time.
 *  - While a song plays there is a big pause button on screen. Touch it and
 *    the music pauses and the device listens: say "resume" to keep playing
 *    or "back to homescreen" to leave song mode and chat again.
 *
 * Libraries: LovyanGFX, ArduinoJson, ESP8266Audio.
 * Board: ESP32-S3 with PSRAM enabled.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <driver/i2s_std.h>
#include <LovyanGFX.hpp>
#include "AudioFileSourceHTTPStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutput.h"

// ==========================================================================
//  FILL THESE THREE IN, then upload this single .ino file. That's it.
//  (No API keys go here - the Gemini key lives only on the server.)
// ==========================================================================
const char* WIFI_SSID   = "YOUR_SSID_HERE";       // <-- your WiFi name here
const char* WIFI_PASS   = "YOUR_PASSWORD_HERE";   // <-- your WiFi password here
const char* SERVER_BASE = "https://raenyxbot.onrender.com";  // <-- your Render server URL here (no trailing slash)

#define GREETING "Hello! I'm Raenyx, your voice assistant. Tell me, what would you like to know?"

#define PIN_MIC_SCK   4
#define PIN_MIC_WS    5
#define PIN_MIC_SD    6
#define PIN_SPK_BCLK  7
#define PIN_SPK_LRC   8
#define PIN_SPK_DIN   17

#define MIC_SHIFT       14
#define VAD_MIN         60
#define VAD_NOISE_MULT  3.0f
#define SPK_GAIN        0.8f
#define MIC_RATE        16000
#define CHUNK           512
#define PREROLL         2048
#define MAX_SAMPLES     160000
#define END_SILENCE     28
#define MIN_VOICED      6
#define WAIT_MS         12000
#define HEAD_RESERVE    768
#define WAV_HDR         44
#define TTS_CAP         700000   // PSRAM buffer for one TTS sentence (WAV)
#define JPG_CAP         250000   // PSRAM buffer for a song background JPG
#define LRC_CAP         24000    // PSRAM buffer for an .lrc file

// ---------------------------------------------------------------- display --
typedef lgfx::Panel_ST7789 PanelType;
#define PANEL_INVERT true

class LGFX : public lgfx::LGFX_Device {
  PanelType _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Touch_XPT2046 _touch;
public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host = SPI2_HOST;
      c.spi_mode = 0;
      c.freq_write = 16000000;
      c.freq_read = 16000000;
      c.pin_sclk = 12;
      c.pin_mosi = 11;
      c.pin_miso = 13;
      c.pin_dc = 9;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.use_lock = true;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = 10;
      c.pin_rst = 14;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.offset_x = 0;
      c.offset_y = 0;
      c.readable = true;
      c.invert = PANEL_INVERT;
      c.rgb_order = false;
      c.dlen_16bit = false;
      c.bus_shared = true;
      _panel.config(c);
    }
    {
      auto c = _touch.config();
      c.x_min = 300;
      c.x_max = 3900;
      c.y_min = 300;
      c.y_max = 3900;
      c.pin_int = -1;
      c.bus_shared = true;
      c.offset_rotation = 0;
      c.spi_host = SPI2_HOST;
      c.freq = 1000000;
      c.pin_sclk = 12;
      c.pin_mosi = 11;
      c.pin_miso = 13;
      c.pin_cs = 15;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

// ----------------------------------------------------------------- states --
enum FaceState { S_IDLE, S_LISTEN, S_THINK, S_SPEAK, S_ERROR };
enum UiMode { UI_FACE, UI_SONG };

LGFX tft;
lgfx::LGFX_Sprite canvas(&tft);
lgfx::LGFX_Sprite bgSpr(&tft);       // song background, drawn once per song

volatile int gState = S_IDLE;
volatile int gUiMode = UI_FACE;
volatile float micLevel = 0;
volatile float mouthTarget = 0;      // lip sync: set by audio playback
volatile bool touchFlag = false;
const char* volatile statusText = "";

uint8_t* recBuf = nullptr;
uint8_t* ttsBuf = nullptr;
uint8_t* jpgBuf = nullptr;
uint8_t* lrcBuf = nullptr;
float noiseFloor = 15;
bool chatActive = false;

struct Msg { String role; String content; };
std::vector<Msg> hist;

i2s_chan_handle_t spkTx = NULL;
i2s_chan_handle_t micRx = NULL;

// ------------------------------------------------------------- audio out --
class NewI2SOut : public AudioOutput {
public:
  int16_t fb[256];
  int fn = 0;

  bool begin() override { return true; }

  bool SetRate(int hz) override {
    if (hz == (int)hertz) return true;
    hertz = hz;
    if (!spkTx) return true;
    i2s_channel_disable(spkTx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)hz);
    esp_err_t e = i2s_channel_reconfig_std_clock(spkTx, &clk);
    i2s_channel_enable(spkTx);
    Serial.printf("[SPK] rate %d Hz: %s\n", hz, esp_err_to_name(e));
    return e == ESP_OK;
  }

  void drain() {
    if (!fn || !spkTx) { fn = 0; return; }
    size_t w = 0;
    esp_err_t e = i2s_channel_write(spkTx, fb, fn * 4, &w, 1000);
    if (e != ESP_OK) Serial.printf("[SPK] write err: %s\n", esp_err_to_name(e));
    fn = 0;
  }

  bool ConsumeSample(int16_t s[2]) override {
    // Lip sync: follow the loudness of what is actually playing.
    int amp = abs(s[0]);
    float lvl = amp / 14000.0f;
    if (lvl > 1.0f) lvl = 1.0f;
    if (lvl > mouthTarget || fn == 0) mouthTarget = lvl;
    int16_t l = (int16_t)(s[0] * SPK_GAIN);
    int16_t r = (channels == 1) ? l : (int16_t)(s[1] * SPK_GAIN);
    fb[fn * 2] = l;
    fb[fn * 2 + 1] = r;
    fn++;
    if (fn >= 128) drain();
    return true;
  }

  bool stop() override { drain(); mouthTarget = 0; return true; }
};

NewI2SOut* audioOut = nullptr;

// Startup beep: two soft tones through the speaker path the moment the
// speaker is ready — before WiFi/server — so a beep at power-on means
// "board + speaker OK", and silence points at the speaker/amp/wiring.
void startupBeep() {
  if (!audioOut) return;
  const int rate = 16000;
  const float tones[2] = {880.0f, 1318.5f};  // A5 -> E6 "ready" chirp
  const int durs[2] = {110, 170};            // ms per tone
  for (int t = 0; t < 2; t++) {
    int n = rate * durs[t] / 1000;
    int fadeIn = rate / 100;                 // 10 ms, avoids a click
    int fadeOut = rate / 50;                 // 20 ms
    for (int i = 0; i < n; i++) {
      float ph = 2.0f * (float)M_PI * tones[t] * i / rate;
      float env = 1.0f;
      if (i < fadeIn) env = (float)i / fadeIn;
      else if (i > n - fadeOut) env = (float)(n - i) / fadeOut;
      int16_t v = (int16_t)(sinf(ph) * 11000.0f * env);
      int16_t s[2] = {v, v};
      audioOut->ConsumeSample(s);
    }
  }
  audioOut->stop();
}

void spkInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  cc.auto_clear = true;
  esp_err_t e = i2s_new_channel(&cc, &spkTx, NULL);
  Serial.printf("[SPK] new_channel: %s\n", esp_err_to_name(e));

  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PIN_SPK_BCLK,
      .ws = (gpio_num_t)PIN_SPK_LRC,
      .dout = (gpio_num_t)PIN_SPK_DIN,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = { false, false, false }
    }
  };
  e = i2s_channel_init_std_mode(spkTx, &sc);
  Serial.printf("[SPK] init_std: %s\n", esp_err_to_name(e));
  e = i2s_channel_enable(spkTx);
  Serial.printf("[SPK] enable: %s\n", esp_err_to_name(e));
}

void setStatus(const char* s) { statusText = s; }

// ----------------------------------------------------------------- songs --
struct Song {
  String id, title, artist;
  std::vector<String> aliases;   // normalized names the voice matcher accepts
  int32_t offsetMs = 0;
  float durationSec = 0;
  String mp3Url, lrcUrl, imgUrl;
};
struct LyricLine { uint32_t ms; String text; };

std::vector<Song> songs;
std::vector<LyricLine> lyrics;
int curSong = -1;
volatile bool songActive = false;
volatile bool songPaused = false;
volatile bool songStopReq = false;
volatile bool songPauseListen = false;  // paused by touch: main loop listens
volatile uint32_t songPosShared = 0;    // playback position for the UI
bool bgReady = false;

// ------------------------------------------------------------- cute face --
void drawFace(uint32_t t) {
  static uint32_t nextBlink = 2000, blinkStart = 0;
  if (t > nextBlink) { blinkStart = t; nextBlink = t + random(2500, 5500); }
  float blink = 1.0f;
  if (t - blinkStart < 160) blink = fabsf(cosf((t - blinkStart) / 160.0f * PI));

  // Lip sync smoothing: playback sets mouthTarget, we ease towards it
  // and let it decay so the mouth closes between words.
  static float mouthSmooth = 0;
  float mt = mouthTarget;
  mouthTarget = mt * 0.72f;
  mouthSmooth += (mt - mouthSmooth) * 0.55f;

  int st = gState;
  uint16_t bg = canvas.color565(10, 12, 24);
  uint16_t eyeCol, mouthCol;
  float eh = 78, ew = 58;
  float dx = 0, dy = 0;

  switch (st) {
    case S_LISTEN:
      eyeCol = canvas.color565(90, 225, 255);
      mouthCol = canvas.color565(90, 225, 255);
      eh = 88;
      break;
    case S_THINK:
      eyeCol = canvas.color565(255, 214, 90);
      mouthCol = canvas.color565(255, 214, 90);
      dx = 13 * sinf(t / 350.0f);
      dy = -6;
      break;
    case S_SPEAK:
      eyeCol = canvas.color565(130, 255, 180);
      mouthCol = canvas.color565(130, 255, 180);
      break;
    case S_ERROR:
      eyeCol = canvas.color565(255, 95, 95);
      mouthCol = canvas.color565(255, 95, 95);
      break;
    default:
      eyeCol = canvas.color565(130, 155, 200);
      mouthCol = canvas.color565(130, 155, 200);
      eh = 14 + 4 * sinf(t / 700.0f);
      break;
  }

  canvas.fillScreen(bg);
  // soft glow behind the face
  canvas.fillCircle(160, 118, 104, canvas.color565(16, 20, 38));

  int cx[2] = {100, 220};
  int ey = 88;
  float h = eh * blink;
  if (h < 7) h = 7;
  for (int i = 0; i < 2; i++) {
    int r = (int)(h < ew ? h : ew) / 2;
    canvas.fillRoundRect(cx[i] - (int)ew / 2, ey - (int)h / 2, (int)ew, (int)h, r, eyeCol);
    if (h > 42) {
      // sparkle highlights - the "cute" part
      canvas.fillCircle(cx[i] + (int)dx - 9, ey + (int)dy - 10, 8, 0xFFFF);
      canvas.fillCircle(cx[i] + (int)dx + 8, ey + (int)dy + 7, 4, 0xFFFF);
      canvas.fillCircle(cx[i] + (int)dx, ey + (int)dy, 13, bg);
      canvas.fillCircle(cx[i] + (int)dx - 9, ey + (int)dy - 10, 7, 0xFFFF);
      canvas.fillCircle(cx[i] + (int)dx + 8, ey + (int)dy + 7, 3, 0xFFFF);
    }
  }

  // rosy cheeks
  uint16_t blush = canvas.color565(112, 52, 74);
  canvas.fillCircle(48, 138, 13, blush);
  canvas.fillCircle(272, 138, 13, blush);

  if (st == S_SPEAK) {
    float o = mouthSmooth;
    if (o < 0.04f) o = 0.04f + 0.03f * fabsf(sinf(t * 0.01f));
    canvas.fillEllipse(160, 182, 20 + (int)(8 * o), 4 + (int)(24 * o), mouthCol);
  } else if (st == S_LISTEN) {
    float ml = micLevel;
    canvas.fillEllipse(160, 182, 10 + (int)(8 * ml), 5 + (int)(13 * ml), mouthCol);
  } else if (st == S_THINK) {
    canvas.fillRoundRect(138, 178, 44, 7, 3, mouthCol);
  } else if (st == S_ERROR) {
    canvas.fillArc(160, 208, 34, 27, 215, 325, mouthCol);
  } else {
    canvas.fillArc(160, 148, 40, 32, 40, 140, mouthCol);  // small smile
  }

  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(canvas.color565(150, 160, 185), bg);
  canvas.setTextDatum(lgfx::bottom_center);
  canvas.drawString((const char*)statusText, 160, 238);
  canvas.pushSprite(0, 0);
}

// ------------------------------------------------------------ song screen --
int currentLyricIdx(uint32_t pos) {
  int idx = -1;
  for (size_t i = 0; i < lyrics.size(); i++) {
    if (lyrics[i].ms <= pos) idx = (int)i; else break;
  }
  return idx;
}

void drawSongScreen(uint32_t t) {
  if (bgReady) bgSpr.pushSprite(&canvas, 0, 0);
  else canvas.fillScreen(canvas.color565(12, 12, 20));
  // darken so lyrics stay readable over any image
  canvas.fillRectAlpha(0, 0, 320, 240, 120, 0x0000);

  const Song* s = (curSong >= 0) ? &songs[curSong] : nullptr;
  uint32_t pos = songPosShared;
  int li = currentLyricIdx(pos);

  canvas.setTextDatum(lgfx::top_center);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(0xFFFF);
  if (s) {
    String head = s->title;
    if (s->artist.length()) head += " - " + s->artist;
    canvas.drawString(head, 160, 8);
  }

  // current line big, next line small
  canvas.setTextDatum(lgfx::middle_center);
  if (li >= 0) {
    String line = lyrics[li].text;
    canvas.setTextSize(line.length() > 26 ? 1 : 2);
    canvas.setTextColor(0xFFFF);
    canvas.drawString(line, 160, 96);
    canvas.setTextSize(1);
    if (li + 1 < (int)lyrics.size()) {
      canvas.setTextColor(canvas.color565(170, 175, 190));
      canvas.drawString(lyrics[li + 1].text, 160, 138);
    }
  } else {
    canvas.setTextSize(2);
    canvas.setTextColor(0xFFFF);
    canvas.drawString("...", 160, 96);
    canvas.setTextSize(1);
  }

  // progress bar
  if (s && s->durationSec > 1) {
    float f = (pos / 1000.0f) / s->durationSec;
    if (f > 1) f = 1;
    canvas.fillRoundRect(30, 162, 260, 6, 3, canvas.color565(70, 74, 90));
    canvas.fillRoundRect(30, 162, (int)(260 * f), 6, 3, 0xFFFF);
  }

  // pause / play button (touch target: centre 160,202 radius ~34)
  canvas.fillCircle(160, 202, 26, canvas.color565(235, 238, 245));
  if (songPaused) {
    canvas.fillTriangle(152, 190, 152, 214, 174, 202, canvas.color565(20, 20, 30));
    canvas.setTextDatum(lgfx::bottom_center);
    canvas.setTextColor(0xFFFF);
    canvas.setFont(&fonts::Font2);
    canvas.drawString("Paused - say resume or back to homescreen", 160, 168 + 8);
  } else {
    canvas.fillRoundRect(149, 190, 8, 24, 2, canvas.color565(20, 20, 30));
    canvas.fillRoundRect(163, 190, 8, 24, 2, canvas.color565(20, 20, 30));
  }
  canvas.pushSprite(0, 0);
}

void faceTask(void*) {
  bool wasDown = false;
  uint32_t lastTouch = 0;
  for (;;) {
    int32_t tx, ty;
    bool down = tft.getTouch(&tx, &ty);
    if (down && !wasDown && millis() - lastTouch > 400) {
      lastTouch = millis();
      if (gUiMode == UI_SONG) {
        int ddx = tx - 160, ddy = ty - 202;
        if (ddx * ddx + ddy * ddy < 34 * 34) {
          if (!songPaused) { songPaused = true; songPauseListen = true; }
          else { songPaused = false; songPauseListen = false; }
        }
      } else {
        touchFlag = true;
      }
    }
    wasDown = down;
    if (gUiMode == UI_SONG) drawSongScreen(millis());
    else drawFace(millis());
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

// -------------------------------------------------------------------- mic --
void micInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  i2s_new_channel(&cc, NULL, &micRx);

  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PIN_MIC_SCK,
      .ws = (gpio_num_t)PIN_MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)PIN_MIC_SD,
      .invert_flags = { false, false, false }
    }
  };
  sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  i2s_channel_init_std_mode(micRx, &sc);
  i2s_channel_enable(micRx);
}

float readChunk(int16_t* dst) {
  static int32_t raw[CHUNK];
  static int32_t v[CHUNK];
  size_t br = 0;
  i2s_channel_read(micRx, raw, sizeof(raw), &br, 1000);
  int n = br / 4;
  if (n <= 0) return 0;
  int64_t sum = 0;
  for (int i = 0; i < n; i++) {
    v[i] = raw[i] >> MIC_SHIFT;
    sum += v[i];
  }
  int32_t mean = sum / n;
  double sq = 0;
  for (int i = 0; i < n; i++) {
    int32_t x = v[i] - mean;
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    dst[i] = (int16_t)x;
    sq += (double)x * x;
  }
  for (int i = n; i < CHUNK; i++) dst[i] = 0;
  return sqrt(sq / n);
}

void flushMic(int chunks) {
  static int16_t tmp[CHUNK];
  for (int i = 0; i < chunks; i++) readChunk(tmp);
}

// Returns sample count, 0 = nothing heard, -1 = touch abort.
int recordUtterance() {
  int16_t* pcm = (int16_t*)(recBuf + HEAD_RESERVE + WAV_HDR);
  size_t pos = 0;
  bool speaking = false;
  int voiced = 0, voicedTotal = 0, silent = 0;
  uint32_t start = millis();
  micLevel = 0;

  while (true) {
    if (touchFlag) { touchFlag = false; return -1; }
    float r = readChunk(pcm + pos);
    float thr = noiseFloor * VAD_NOISE_MULT + 30;
    if (thr < VAD_MIN) thr = VAD_MIN;
    bool v = r > thr;
    micLevel = min(1.0f, r / (thr * 5));
    if (!v && !speaking) noiseFloor = noiseFloor * 0.95f + r * 0.05f;
    if (noiseFloor > 1200) noiseFloor = 1200;

    if (!speaking) {
      pos += CHUNK;
      voiced = v ? voiced + 1 : 0;
      if (pos > PREROLL) {
        memmove(pcm, pcm + pos - PREROLL, PREROLL * 2);
        pos = PREROLL;
      }
      if (voiced >= 2) { speaking = true; silent = 0; voicedTotal = voiced; }
      if (millis() - start > WAIT_MS) return 0;
    } else {
      pos += CHUNK;
      if (v) { silent = 0; voicedTotal++; } else silent++;
      if (silent >= END_SILENCE) break;
      if (pos + CHUNK > MAX_SAMPLES) break;
    }
  }
  micLevel = 0;
  if (voicedTotal < MIN_VOICED) return 0;
  return (int)pos;
}

void writeWavHeader(uint8_t* h, uint32_t dataBytes) {
  uint32_t rate = MIC_RATE, byteRate = MIC_RATE * 2, chunk = 36 + dataBytes;
  uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
  uint32_t fmtLen = 16;
  memcpy(h, "RIFF", 4);
  memcpy(h + 4, &chunk, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  memcpy(h + 16, &fmtLen, 4);
  memcpy(h + 20, &fmt, 2);
  memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &rate, 4);
  memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &align, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &dataBytes, 4);
}

// ------------------------------------------------------------- HTTP bits --
class MemSink : public Stream {
public:
  uint8_t* b;
  size_t cap;
  size_t len = 0;
  MemSink(uint8_t* buf, size_t c) : b(buf), cap(c) {}
  size_t write(uint8_t c) override {
    if (len < cap) { b[len++] = c; return 1; }
    return 0;
  }
  size_t write(const uint8_t* d, size_t n) override {
    size_t m = (cap - len) < n ? (cap - len) : n;
    memcpy(b + len, d, m);
    len += m;
    return m;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

String serverUrl(const String& path) { return String(SERVER_BASE) + path; }

// GET into a PSRAM buffer (for .lrc / .jpg). Returns bytes read or -1.
int httpGetToBuffer(const String& url, uint8_t* buf, size_t cap) {
  WiFiClientSecure cl;
  cl.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.begin(cl, url);
  int code = http.GET();
  if (code != 200) { http.end(); return -1; }
  MemSink sink(buf, cap);
  http.writeToStream(&sink);
  http.end();
  return (int)sink.len;
}

bool httpGetString(const String& url, String& out) {
  WiFiClientSecure cl;
  cl.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.begin(cl, url);
  int code = http.GET();
  if (code == 200) out = http.getString();
  http.end();
  return code == 200;
}

// ------------------------------------------------------- server: STT/LLM --
bool transcribe(int samples, String& text) {
  size_t pcmBytes = (size_t)samples * 2;
  writeWavHeader(recBuf + HEAD_RESERVE, pcmBytes);

  const String B = "----raenyxbot";
  String head;
  head += "--" + B + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + B + "--\r\n";
  size_t hl = head.length();
  if (hl > HEAD_RESERVE) return false;
  uint8_t* startPtr = recBuf + HEAD_RESERVE - hl;
  memcpy(startPtr, head.c_str(), hl);
  memcpy(recBuf + HEAD_RESERVE + WAV_HDR + pcmBytes, tail.c_str(), tail.length());
  size_t total = hl + WAV_HDR + pcmBytes + tail.length();

  WiFiClientSecure cl;
  cl.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(cl, serverUrl("/api/transcribe"));
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + B);
  int code = http.POST(startPtr, total);
  if (code != 200) { http.end(); return false; }
  String resp = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  text = String((const char*)(doc["text"] | ""));
  text.trim();
  return true;
}

bool askServer(const String& q, String& answer) {
  hist.push_back({"user", q});
  while (hist.size() > 8) hist.erase(hist.begin());
  while (!hist.empty() && hist.front().role != "user") hist.erase(hist.begin());

  JsonDocument doc;
  doc["text"] = q;
  JsonArray h = doc["history"].to<JsonArray>();
  for (size_t i = 0; i + 1 < hist.size(); i++) {
    JsonObject o = h.add<JsonObject>();
    o["role"] = hist[i].role;
    o["content"] = hist[i].content;
  }
  String body;
  serializeJson(doc, body);

  WiFiClientSecure cl;
  cl.setInsecure();
  HTTPClient http;
  http.setTimeout(45000);
  http.begin(cl, serverUrl("/api/chat"));
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code != 200) { http.end(); hist.pop_back(); return false; }
  String resp = http.getString();
  http.end();

  JsonDocument r;
  if (deserializeJson(r, resp)) { hist.pop_back(); return false; }
  answer = String((const char*)(r["reply"] | ""));
  answer.trim();
  if (answer.length() == 0) { hist.pop_back(); return false; }
  hist.push_back({"assistant", answer});
  return true;
}

// --------------------------------------------------------- server: speak --
// Play a WAV held in memory (Gemini TTS: 24 kHz 16-bit mono PCM in WAV).
bool playWav(const uint8_t* buf, size_t len) {
  if (len < WAV_HDR + 100) return false;
  uint32_t rate = buf[24] | (buf[25] << 8) | (buf[26] << 16) | ((uint32_t)buf[27] << 24);
  // find the "data" chunk (canonical WAVs from our server: offset 44)
  size_t off = 12;
  size_t dataOff = WAV_HDR;
  while (off + 8 <= len) {
    uint32_t sz = buf[off + 4] | (buf[off + 5] << 8) | (buf[off + 6] << 16) | ((uint32_t)buf[off + 7] << 24);
    if (memcmp(buf + off, "data", 4) == 0) { dataOff = off + 8; break; }
    off += 8 + sz;
  }
  audioOut->SetRate(rate ? (int)rate : 24000);
  audioOut->SetChannels(1);
  const int16_t* pcm = (const int16_t*)(buf + dataOff);
  size_t n = (len - dataOff) / 2;
  int16_t smp[2];
  for (size_t i = 0; i < n; i++) {
    smp[0] = pcm[i];
    smp[1] = pcm[i];
    audioOut->ConsumeSample(smp);
    if (touchFlag) { audioOut->stop(); return false; }
    if ((i & 0x3FF) == 0) yield();
  }
  audioOut->stop();
  return true;
}

std::vector<String> splitText(const String& s) {
  std::vector<String> out;
  String cur;
  int cnt = 0;
  for (size_t i = 0; i < s.length();) {
    uint8_t c = s[i];
    int n = c < 0x80 ? 1 : ((c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : 4));
    String ch = s.substring(i, i + n);
    i += n;
    cur += ch;
    cnt++;
    bool sentEnd = (ch == "।" || ch == "." || ch == "?" || ch == "!" || ch == "\n");
    if ((sentEnd && cnt >= 25) || cnt >= 110) {
      cur.trim();
      if (cur.length()) out.push_back(cur);
      cur = "";
      cnt = 0;
    }
  }
  cur.trim();
  if (cur.length()) out.push_back(cur);
  return out;
}

bool speak(const String& text) {
  std::vector<String> parts = splitText(text);
  gState = S_SPEAK;
  setStatus("Speaking...");
  for (auto& p : parts) {
    if (touchFlag) return false;
    JsonDocument doc;
    doc["text"] = p;
    String body;
    serializeJson(doc, body);

    MemSink sink(ttsBuf, TTS_CAP);
    WiFiClientSecure cl;
    cl.setInsecure();
    HTTPClient http;
    http.setTimeout(20000);
    http.begin(cl, serverUrl("/api/tts"));
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    if (code == 200) http.writeToStream(&sink);
    http.end();
    if (code != 200 || sink.len < 500) {
      Serial.printf("tts failed code %d len %u\n", code, (unsigned)sink.len);
      continue;
    }
    if (!playWav(ttsBuf, sink.len)) return false;
  }
  return true;
}

// -------------------------------------------------------------- song mode --
String normalizeVoice(const String& s) {
  String o;
  bool lastSpace = true;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { o += c; lastSpace = false; }
    else if (!lastSpace) { o += ' '; lastSpace = true; }
  }
  o.trim();
  return o;
}

bool fetchSongs() {
  String resp;
  if (!httpGetString(serverUrl("/songs.json"), resp)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  songs.clear();
  for (JsonObject o : doc["songs"].as<JsonArray>()) {
    Song s;
    s.id = String((const char*)(o["id"] | ""));
    s.title = String((const char*)(o["title"] | ""));
    s.artist = String((const char*)(o["artist"] | ""));
    s.offsetMs = o["lrc_offset_ms"] | 0;
    s.durationSec = o["duration_sec"] | 0.0f;
    s.mp3Url = serverUrl(String((const char*)(o["mp3_url"] | "")));
    const char* l = o["lrc_url"];
    if (l) s.lrcUrl = serverUrl(String(l));
    const char* im = o["image_url"];
    if (im) s.imgUrl = serverUrl(String(im));
    for (const char* a : o["aliases"].as<JsonArray>()) s.aliases.push_back(String(a));
    if (s.id.length()) songs.push_back(s);
  }
  Serial.printf("[SONGS] %u loaded\n", (unsigned)songs.size());
  return true;
}

// "play the song blue" / "blue by yung kai" -> song index or -1
int matchSong(const String& text) {
  String n = normalizeVoice(text);
  bool wantsPlay = n.indexOf("play") >= 0 || n.indexOf("bajao") >= 0 ||
                   n.indexOf("chalao") >= 0 || n.indexOf("song") >= 0 ||
                   n.indexOf("gan") >= 0;
  int best = -1, bestLen = 0;
  for (size_t i = 0; i < songs.size(); i++) {
    for (auto& a : songs[i].aliases) {
      if (a.length() >= 3 && n.indexOf(a) >= 0 && (int)a.length() > bestLen) {
        if (wantsPlay || n == a) { best = (int)i; bestLen = a.length(); }
      }
    }
  }
  return best;
}

void parseLrc(const uint8_t* buf, size_t len) {
  lyrics.clear();
  String all;
  all.reserve(len + 1);
  for (size_t i = 0; i < len; i++) all += (char)buf[i];
  int start = 0;
  while (start < (int)all.length()) {
    int nl = all.indexOf('\n', start);
    if (nl < 0) nl = all.length();
    String line = all.substring(start, nl);
    start = nl + 1;
    // one line can carry several [mm:ss.xx] tags; the text follows the last ]
    int close = line.lastIndexOf(']');
    if (close < 0) continue;
    String txt = line.substring(close + 1);
    txt.trim();
    int p = 0;
    while (true) {
      int ob = line.indexOf('[', p);
      if (ob < 0 || ob >= close) break;
      int cb = line.indexOf(']', ob);
      String tag = line.substring(ob + 1, cb);
      int colon = tag.indexOf(':');
      if (colon > 0) {
        String mm = tag.substring(0, colon);
        String ss = tag.substring(colon + 1);
        bool numeric = mm.length() && isdigit(mm[0]) && ss.length() && isdigit(ss[0]);
        if (numeric) {
          float sec = ss.toFloat();
          uint32_t ms = (uint32_t)(mm.toInt() * 60000 + sec * 1000.0f);
          lyrics.push_back({ms, txt});
        }
      }
      p = cb + 1;
    }
  }
  // simple insertion sort by time (lrc files are nearly sorted already)
  for (size_t i = 1; i < lyrics.size(); i++) {
    LyricLine v = lyrics[i];
    int j = (int)i - 1;
    while (j >= 0 && lyrics[j].ms > v.ms) { lyrics[j + 1] = lyrics[j]; j--; }
    lyrics[j + 1] = v;
  }
  Serial.printf("[LRC] %u lines\n", (unsigned)lyrics.size());
}

void songTask(void* param) {
  int idx = (int)(intptr_t)param;
  String url = songs[idx].mp3Url;
  AudioFileSourceHTTPStream* src = new AudioFileSourceHTTPStream(url.c_str());
  AudioGeneratorMP3* mp3 = new AudioGeneratorMP3();
  mp3->begin(src, audioOut);
  uint32_t startRef = millis();
  uint32_t pauseAccum = 0, pausedAt = 0;
  bool wasPaused = false;

  while (!songStopReq) {
    if (songPaused) {
      if (!wasPaused) { wasPaused = true; pausedAt = millis(); }
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }
    if (wasPaused) { pauseAccum += millis() - pausedAt; wasPaused = false; }
    if (mp3->isRunning()) {
      if (!mp3->loop()) mp3->stop();
      long p = (long)(millis() - startRef - pauseAccum) + songs[idx].offsetMs;
      songPosShared = p < 0 ? 0 : (uint32_t)p;
    } else break;
  }
  mp3->stop();
  delete mp3;
  delete src;
  audioOut->stop();
  songActive = false;
  songPaused = false;
  songPauseListen = false;
  songPosShared = 0;
  curSong = -1;
  gUiMode = UI_FACE;
  gState = S_IDLE;
  setStatus("Touch to start live chat");
  vTaskDelete(NULL);
}

void stopSong() {
  if (!songActive) return;
  songStopReq = true;
  uint32_t t0 = millis();
  while (songActive && millis() - t0 < 3000) delay(50);
}

bool startSong(int idx) {
  stopSong();
  gState = S_THINK;
  setStatus("Loading song...");
  curSong = idx;

  lyrics.clear();
  if (songs[idx].lrcUrl.length()) {
    int n = httpGetToBuffer(songs[idx].lrcUrl, lrcBuf, LRC_CAP);
    if (n > 0) parseLrc(lrcBuf, n);
  }
  bgReady = false;
  if (songs[idx].imgUrl.length()) {
    int n = httpGetToBuffer(songs[idx].imgUrl, jpgBuf, JPG_CAP);
    if (n > 0) {
      bgSpr.fillScreen(0);
      bgReady = bgSpr.drawJpg(jpgBuf, n, 0, 0);
    }
  }
  songStopReq = false;
  songPaused = false;
  songPauseListen = false;
  songPosShared = 0;
  songActive = true;
  gUiMode = UI_SONG;
  xTaskCreatePinnedToCore(songTask, "song", 8192, (void*)(intptr_t)idx, 1, NULL, 1);
  return true;
}

// While paused by touch, listen for "resume" / "back to homescreen".
void handlePausedCommand() {
  static int attempts = 0;
  if (!songPaused) { attempts = 0; return; }
  if (attempts >= 8) return;  // stop nagging; touch play to resume instead
  gState = S_LISTEN;
  setStatus("Paused - say resume or back to homescreen");
  int n = recordUtterance();
  if (n <= 0) { attempts++; return; }
  gState = S_THINK;
  setStatus("Transcribing...");
  String q;
  if (!transcribe(n, q)) { attempts++; return; }
  Serial.println("Paused cmd: " + q);
  String v = normalizeVoice(q);
  if (v.indexOf("back to home") >= 0 || v.indexOf("homescreen") >= 0 ||
      v.indexOf("home screen") >= 0 || v.indexOf("go back") >= 0) {
    attempts = 0;
    stopSong();
    speak("Okay, I'm back. What would you like to know?");
    flushMic(10);
    return;
  }
  if (v.indexOf("resume") >= 0 || v.indexOf("play again") >= 0 || v == "play") {
    attempts = 0;
    songPaused = false;
    songPauseListen = false;
    return;
  }
  attempts++;
}

// ------------------------------------------------------------------- misc --
void showError(const char* msg) {
  gState = S_ERROR;
  setStatus(msg);
  Serial.println(msg);
  delay(2500);
}

void connectWiFi() {
  gState = S_THINK;
  setStatus("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(WiFi.localIP());
    gState = S_IDLE;
  } else {
    showError("WiFi failed, retrying");
  }
}

void stopChat() {
  chatActive = false;
  touchFlag = false;
  gState = S_IDLE;
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0);

  if (!psramFound()) {
    tft.setTextColor(0xF800);
    tft.drawString("PSRAM not found", 20, 100);
    Serial.println("Enable PSRAM in Tools menu");
    while (true) delay(1000);
  }

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  canvas.createSprite(320, 240);
  bgSpr.setPsram(true);
  bgSpr.setColorDepth(16);
  bgSpr.createSprite(320, 240);

  recBuf = (uint8_t*)ps_malloc(HEAD_RESERVE + WAV_HDR + MAX_SAMPLES * 2 + 256);
  ttsBuf = (uint8_t*)ps_malloc(TTS_CAP);
  jpgBuf = (uint8_t*)ps_malloc(JPG_CAP);
  lrcBuf = (uint8_t*)ps_malloc(LRC_CAP);
  if (!recBuf || !ttsBuf || !jpgBuf || !lrcBuf) {
    Serial.println("PSRAM allocation failed");
    while (true) delay(1000);
  }

  setStatus("Starting...");
  xTaskCreatePinnedToCore(faceTask, "face", 8192, NULL, 1, NULL, 0);

  micInit();
  flushMic(10);
  static int16_t tmp[CHUNK];
  float acc = 0;
  for (int i = 0; i < 16; i++) acc += readChunk(tmp);
  noiseFloor = acc / 16;
  if (noiseFloor > 1200) noiseFloor = 1200;
  Serial.printf("noise floor %.1f\n", noiseFloor);

  spkInit();
  audioOut = new NewI2SOut();
  audioOut->SetRate(16000);
  audioOut->SetChannels(1);
  startupBeep();  // speaker self-test: beep = board + speaker OK

  connectWiFi();
  if (!fetchSongs()) { delay(1500); fetchSongs(); }  // server may be waking up
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }

  // Song mode owns the loop until the song ends or is stopped.
  if (songActive) {
    if (songPauseListen) handlePausedCommand();
    delay(20);
    return;
  }

  if (!chatActive) {
    gState = S_IDLE;
    setStatus("Touch to start live chat");
    if (touchFlag) {
      touchFlag = false;
      chatActive = true;
      hist.clear();
      if (!speak(GREETING)) { stopChat(); return; }
      flushMic(10);
    }
    delay(20);
    return;
  }

  gState = S_LISTEN;
  setStatus("Listening... (touch to stop)");
  int n = recordUtterance();
  if (n < 0) { stopChat(); return; }
  if (n == 0) return;

  gState = S_THINK;
  setStatus("Transcribing...");
  String q;
  if (!transcribe(n, q)) { showError("STT error"); return; }
  Serial.println("You: " + q);
  if (q.length() < 2) return;

  // Song commands are checked BEFORE the chat brain.
  int si = matchSong(q);
  if (si >= 0) {
    Serial.printf("Song command -> %s\n", songs[si].title.c_str());
    startSong(si);
    return;
  }

  setStatus("Thinking...");
  String a;
  if (!askServer(q, a)) { showError("AI error"); return; }
  Serial.println("AI: " + a);

  if (!speak(a)) { stopChat(); return; }
  flushMic(10);
}
