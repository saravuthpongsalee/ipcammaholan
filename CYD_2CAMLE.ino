
const char* WIFI_SSID = "RINDA 2.4G";          
const char* WIFI_PASS = "0910318833";      

const char* CAM1_NAME = "cam1";  // กล้องที่ 1 
const char* CAM1_URL  = "https://cctv.maholan.net/api/snap/samui-7?t=59772508";


const char* CAM2_NAME = "cam2";        // กล้องที่ 2 
const char* CAM2_URL  = "https://cctv.maholan.net/api/snap/nakhon-sc025-sub?t=1195451118";

const unsigned long REFRESH_MS = 30000;
const int MAX_ATTEMPTS = 3;
const bool REJECT_WHITE_IMAGE = true;
#define USE_ILI9341 0
const int LCD_ROTATION = 1;

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <esp_heap_caps.h>
#include <time.h>
#include <math.h>

class CYDDisplay : public lgfx::LGFX_Device {
  lgfx::Bus_SPI bus;
#if USE_ILI9341
  lgfx::Panel_ILI9341 panel;
#else
  lgfx::Panel_ST7789 panel;
#endif
  lgfx::Light_PWM light;

public:
  CYDDisplay() {
    auto b = bus.config();
    b.spi_host = HSPI_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.spi_3wire = true;
    b.use_lock = true;
    b.dma_channel = 1;
    b.pin_sclk = 14;
    b.pin_mosi = 13;
    b.pin_miso = 12;
    b.pin_dc = 2;
    bus.config(b);
    panel.setBus(&bus);

    auto p = panel.config();
    p.pin_cs = 15;
    p.pin_rst = -1;
    p.pin_busy = -1;
    p.memory_width = 240;
    p.memory_height = 320;
    p.panel_width = 240;
    p.panel_height = 320;
    p.offset_x = 0;
    p.offset_y = 0;
    p.offset_rotation = 0;
    p.readable = false;
    p.invert = false;
    p.rgb_order = false;
    p.bus_shared = true;
    panel.config(p);

    auto l = light.config();
    l.pin_bl = 21;
    l.freq = 44100;
    l.pwm_channel = 7;
    light.config(l);
    panel.setLight(&light);
    setPanel(&panel);
  }
};

CYDDisplay lcd;
LGFX_Sprite tile(&lcd);
const size_t MAX_JPEG = 65536;
const int TILE_W = 156;
const int TILE_H = 151;
uint8_t* jpegBuffer = nullptr;
unsigned long lastRound = 0;
unsigned long lastWiFiTry = 0;
bool firstRound = true;
bool wasConnected = false;
bool hasPicture[2] = {false, false};
const char* rootCertificates();

class JpegReceiver : public Stream {
public:
  size_t used = 0;
  bool overflow = false;
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t length) override {
    if (length > MAX_JPEG - used) {
      overflow = true;
      return 0;
    }
    memcpy(jpegBuffer + used, data, length);
    used += length;
    return length;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

void printArea(const String& message, int x, int y, int width, uint16_t color) {
  lcd.fillRect(x, y, width, 12, 0x0000);
  lcd.setClipRect(x, y, width, 12);
  lcd.setTextColor(color, 0x0000);
  lcd.setCursor(x, y);
  lcd.print(message);
  lcd.clearClipRect();
}

void showNetwork(const String& message) {
  printArea(message, 2, 226, 316, 0xFFFF);
}

void showCameraStatus(int camera, const String& message, bool ok) {
  printArea(message, camera * 160 + 3, 204, 154, ok ? 0x07E0 : 0xF800);
}

void drawLayout() {
  lcd.fillScreen(0x0000);
  lcd.setTextSize(1);
  lcd.setTextWrap(false);
  printArea("CYD 2 CAM | V2.1 | 28/10/2026", 2, 3, 316, 0xFFFF);
  printArea(String("1: ") + CAM1_NAME, 3, 25, 154, 0xFFFF);
  printArea(String("2: ") + CAM2_NAME, 163, 25, 154, 0xFFFF);
  lcd.drawRect(0, 41, 160, 155, 0xFFFF);
  lcd.drawRect(160, 41, 160, 155, 0xFFFF);
  showCameraStatus(0, "Waiting", true);
  showCameraStatus(1, "Waiting", true);
}

bool jpegSize(const uint8_t* data, size_t length, int& width, int& height) {
  if (length < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
  size_t pos = 2;
  while (pos + 4 <= length) {
    if (data[pos++] != 0xFF) continue;
    while (pos < length && data[pos] == 0xFF) pos++;
    if (pos >= length) break;
    uint8_t marker = data[pos++];
    if (marker == 0xD9 || marker == 0xDA) break;
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
    if (pos + 2 > length) break;
    size_t block = (data[pos] << 8) | data[pos + 1];
    if (block < 2 || block > length - pos) break;
    if (marker == 0xC2) return false;
    if (marker == 0xC0 && block >= 8) {
      height = (data[pos + 3] << 8) | data[pos + 4];
      width = (data[pos + 5] << 8) | data[pos + 6];
      return width > 0 && height > 0;
    }
    pos += block;
  }
  return false;
}

bool downloadCamera(int camera, const char* source, size_t& count, String& result, int& httpCode) {
  count = 0;
  httpCode = 0;
  if (WiFi.status() != WL_CONNECTED) {
    result = "Wi-Fi lost";
    return false;
  }

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  String url(source);
  bool https = url.startsWith("https://");
  if (!https && !url.startsWith("http://")) {
    result = "Bad URL";
    return false;
  }
  if (url.startsWith("https://cctv.maholan.net/api/snap/") && url.indexOf('?') < 0) {
    url += "?t=";
    url += String((uint32_t)time(nullptr) / 30);
  }

  secure.setCACert(rootCertificates());
  secure.setHandshakeTimeout(12);
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool opened = https ? http.begin(secure, url) : http.begin(plain, url);
  if (!opened) {
    result = "Bad URL";
    return false;
  }

  const char* keys[] = {"Content-Type", "Content-Encoding"};
  http.collectHeaders(keys, 2);
  http.setUserAgent("CYD-Maholan/1.2");
  http.addHeader("Accept", "image/jpeg");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Cache-Control", "no-cache");
  Serial.printf("[CAM %d] heap=%u largest=%u RSSI=%d\n", camera + 1,
    ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), WiFi.RSSI());

  httpCode = http.GET();
  String type = http.header("Content-Type");
  type.toLowerCase();
  String encoding = http.header("Content-Encoding");
  encoding.toLowerCase();
  int expected = http.getSize();
  Serial.printf("[CAM %d] HTTP=%d length=%d type=%s encoding=%s\n",
    camera + 1, httpCode, expected, type.c_str(), encoding.c_str());
  bool complete = false;

  if (httpCode < 0) {
    result = String("HTTP ") + httpCode;
    char tlsText[160] = {};
    int tlsCode = https ? secure.lastError(tlsText, sizeof(tlsText)) : 0;
    Serial.printf("[CAM %d] NET=%s TLS=%d %s\n", camera + 1,
      HTTPClient::errorToString(httpCode).c_str(), tlsCode, tlsText);
  } else if (httpCode != HTTP_CODE_OK) {
    result = String("HTTP ") + httpCode;
  } else if (expected > (int)MAX_JPEG) {
    result = "JPEG > 64 KiB";
  } else if (type.length() && !type.startsWith("image/jpeg") &&
             !type.startsWith("image/jpg") && !type.startsWith("application/octet-stream")) {
    result = "Not JPEG response";
  } else if (encoding.length() && encoding != "identity") {
    result = "Encoded response";
  } else {
    JpegReceiver receiver;
    int written = http.writeToStream(&receiver);
    count = receiver.used;
    complete = written >= 0 && !receiver.overflow && count >= 4;
    if (expected >= 0 && count != (size_t)expected) complete = false;
    if (complete) {
      complete = jpegBuffer[0] == 0xFF && jpegBuffer[1] == 0xD8 &&
                 jpegBuffer[count - 2] == 0xFF && jpegBuffer[count - 1] == 0xD9;
    }
    result = receiver.overflow ? "JPEG > 64 KiB" : "Incomplete JPEG";
    Serial.printf("[CAM %d] received=%u written=%d complete=%d\n",
      camera + 1, (unsigned)count, written, complete);
  }

  http.end();
  secure.stop();
  plain.stop();
  return complete;
}

bool renderCamera(int camera, size_t count, String& result) {
  int imageW = 0, imageH = 0;
  if (!jpegSize(jpegBuffer, count, imageW, imageH)) {
    result = "Need baseline JPG";
    return false;
  }

  tile.deleteSprite();
  tile.setColorDepth(16);
  tile.setPsram(false);
  if (!tile.createSprite(TILE_W, TILE_H)) {
    result = "No render RAM";
    Serial.printf("[CAM %d] sprite allocation failed heap=%u largest=%u\n", camera + 1,
      ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return false;
  }

  tile.fillSprite(0x0000);
  float scale = fminf((float)TILE_W / imageW, (float)TILE_H / imageH);
  int drawW = (int)floorf(imageW * scale);
  int drawH = (int)floorf(imageH * scale);
  if (drawW < 1) drawW = 1;
  if (drawH < 1) drawH = 1;
  int x = (TILE_W - drawW) / 2;
  int y = (TILE_H - drawH) / 2;
  bool decoded = tile.drawJpg(jpegBuffer, count, x, y, TILE_W - x, TILE_H - y, 0, 0, scale, scale);
  unsigned white = 0, samples = 0;
  if (decoded) {
    for (int py = y; py < y + drawH; py += 3) {
      for (int px = x; px < x + drawW; px += 3) {
        uint16_t pixel = tile.readPixel(px, py);
        if ((pixel >> 11) >= 30 && ((pixel >> 5) & 63) >= 60 && (pixel & 31) >= 30) white++;
        samples++;
      }
    }
  }
  bool blank = decoded && samples && white * 100UL >= samples * 99UL;
  Serial.printf("[CAM %d] JPEG=%dx%d decode=%d white=%u/%u targetX=%d\n",
    camera + 1, imageW, imageH, decoded, white, samples, camera * 160 + 2);

  bool accepted = decoded && !(REJECT_WHITE_IMAGE && blank);
  if (accepted) {
    lcd.clearClipRect();
    tile.pushSprite(camera * 160 + 2, 43);
    hasPicture[camera] = true;
    result = String("RX ") + millis() / 1000 + "s";
  } else {
    result = decoded ? "Source nearly white" : "JPEG decode error";
  }
  tile.deleteSprite();
  return accepted;
}

void updateCamera(int camera, const char* url) {
  String result;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    showCameraStatus(camera, String("Load ") + attempt + "/" + MAX_ATTEMPTS, true);
    Serial.printf("[CAM %d] attempt %d/%d\n", camera + 1, attempt, MAX_ATTEMPTS);
    size_t count = 0;
    int code = 0;
    bool downloaded = downloadCamera(camera, url, count, result, code);
    bool success = downloaded && renderCamera(camera, count, result);
    Serial.printf("[CAM %d] %s oldPicture=%d\n", camera + 1, result.c_str(), hasPicture[camera]);
    if (success) {
      showCameraStatus(camera, result, true);
      return;
    }
    showCameraStatus(camera, result, false);
    if (WiFi.status() != WL_CONNECTED || code == 429 || (code >= 400 && code < 500)) break;
    if (attempt < MAX_ATTEMPTS) delay(1200 * attempt);
  }
}

void setup() {
  Serial.begin(115200);
  lcd.init();
  lcd.setRotation(LCD_ROTATION);
  lcd.setBrightness(255);
  drawLayout();
  jpegBuffer = (uint8_t*)malloc(MAX_JPEG);
  if (!jpegBuffer) {
    showNetwork("Not enough RAM - restart board");
    while (true) delay(1000);
  }
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWiFiTry = millis();
  showNetwork("Connecting Wi-Fi...");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    showNetwork("Wi-Fi disconnected / retrying");
    wasConnected = false;
    if (millis() - lastWiFiTry >= 15000) {
      lastWiFiTry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    delay(100);
    return;
  }
  if (!wasConnected) {
    wasConnected = true;
    firstRound = true;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Serial.print("Wi-Fi IP: ");
    Serial.println(WiFi.localIP());
  }
  bool needsTime = String(CAM1_URL).startsWith("https://") || String(CAM2_URL).startsWith("https://");
  if (needsTime && time(nullptr) < 1700000000) {
    showNetwork("Wi-Fi OK / waiting NTP for HTTPS");
    delay(200);
    return;
  }
  showNetwork(String("Wi-Fi OK | ") + WiFi.localIP().toString());
  if (firstRound || millis() - lastRound >= REFRESH_MS) {
    updateCamera(0, CAM1_URL);
    delay(500);
    if (WiFi.status() == WL_CONNECTED) updateCamera(1, CAM2_URL);
    lastRound = millis();
    firstRound = false;
  }
  delay(50);
}

const char* rootCertificates() {
  return R"CERT(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)CERT";

}
