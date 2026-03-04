#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <base64.h>
#include <TJpg_Decoder.h>

#include "secrets.h"

#ifndef TOUCH_CS
#define TOUCH_CS 5
#endif

TFT_eSPI tft;
XPT2046_Touchscreen touch(TOUCH_CS);
Preferences prefs;

static const char *SPOTIFY_TOKEN_URL  = "https://accounts.spotify.com/api/token";
static const char *SPOTIFY_API_BASE   = "https://api.spotify.com/v1";
static const char *SPOTIFY_AUTH_BASE  = "https://accounts.spotify.com/authorize";
static const char *SPOTIFY_SCOPES     =
    "user-read-playback-state user-modify-playback-state user-library-read user-library-modify";

struct Track {
  String  id, title, artist, album;
  int32_t duration_ms = 0, progress_ms = 0;
  bool    playing = false, saved = false, active = false;
};

Track   curr, prev;
String  g_accessToken;
uint32_t g_tokenExpiresAt = 0;
String  currAlbumArtUrl;

uint32_t lastPollMs   = 0;
uint32_t lastTickMs   = 0;
uint32_t lastTouchMs  = 0;
bool     needsFullRedraw = true;

String   g_lastArtUrl;

#define HDR_Y 0
#define HDR_H 34
#define ART_W 100
#define ART_H 100
#define ART_X 20
#define ART_Y 40
#define INFO_X 230
#define INFO_Y 50
#define INFO_W 240
#define PB_X  8
#define PB_Y  230
#define PB_W  464
#define PB_H  6
#define TIME_Y 240
#define CTRL_Y 285

#define BTN_PREV_X 160
#define BTN_PP_X   240
#define BTN_NEXT_X 320
#define BTN_R_SM   22
#define BTN_R_LG   28

#define C_BG     0x0000
#define C_CARD   0x18C3
#define C_GREEN  0x06c9
#define C_WHITE  0xFFFF
#define C_GRAY   0x8C71
#define C_MUTED  0x4208
#define C_BORDER 0x2104
#define C_RED    0xF800

void saveRefreshToken(const String &rt) {
  prefs.begin("spotify", false);
  prefs.putString("refresh_token", rt);
  prefs.end();
}
String loadRefreshToken() {
  prefs.begin("spotify", false);
  String rt = prefs.getString("refresh_token", "");
  prefs.end();
  return rt;
}
void clearRefreshToken() {
  prefs.begin("spotify", false);
  prefs.remove("refresh_token");
  prefs.end();
}

void drawStatus(const String &msg, uint16_t color = C_GRAY) {
  tft.fillRect(0, HDR_H + 4, 20, 26, C_BG);
  tft.setTextColor(color, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.drawString(msg, 240, HDR_H + 36);
}

void drawHeader(const String &right = "") {
  tft.fillRect(0, HDR_Y, 480, HDR_H, C_GREEN);
  tft.setTextColor(TFT_BLACK, C_GREEN);
  tft.setTextDatum(ML_DATUM);
  tft.setTextSize(2);
  tft.drawString("SPOTIFY", 12, HDR_Y + HDR_H / 2);
  if (right.length()) {
    tft.setTextDatum(MR_DATUM);
    tft.setTextSize(2);
    tft.drawString(right, 480, HDR_Y + HDR_H / 2);
  }
}

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

String fitString(const String &s, uint8_t sz, int maxW) {
  tft.setTextSize(sz);
  if (tft.textWidth(s) <= maxW) return s;
  String out = s;
  while (out.length() > 1 && tft.textWidth(out + "...") > maxW)
    out.remove(out.length() - 1);
  return out + "...";
}

void drawAlbumArt(const String &url) {
  if (!url.length()) {
    tft.fillRect(ART_X, ART_Y, ART_W, ART_H, C_BG);
    return;
  }
  g_lastArtUrl = url;

  tft.fillRect(ART_X, ART_Y, ART_W, ART_H, C_BG);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) { Serial.println("[ART] begin failed"); return; }

  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[ART] HTTP %d\n", code);
    http.end(); return;
  }

  int len = http.getSize();
  Serial.print(len);

  if (len <= 0 || len > 150000) {
    Serial.printf("[ART] bad size %d\n", len);
    http.end(); return;
  }

  uint8_t *jpg = (uint8_t *)malloc(len);
  if (!jpg) { Serial.println("[ART] malloc failed"); http.end(); return; }

  WiFiClient *s   = http.getStreamPtr();
  int total       = 0;
  uint32_t tStart = millis();
  while (total < len && millis() - tStart < 8000) {
    int av = s->available();
    if (av > 0) {
      int n = s->readBytes(jpg + total, min(av, len - total));
      if (n > 0) total += n;
    } else {
      delay(1);
    }
  }
  http.end();

  if (total < len) {
    Serial.printf("[ART] incomplete %d/%d\n", total, len);
    free(jpg); return;
  }

  TJpgDec.setJpgScale(2);
  TJpgDec.setSwapBytes(true);

  uint16_t jpgW = 0, jpgH = 0;
  TJpgDec.getJpgSize(&jpgW, &jpgH, jpg, total);
  Serial.printf("[ART] JPEG size %dx%d\n", jpgW, jpgH);

  JRESULT r = TJpgDec.drawJpg(ART_X, ART_Y, jpg, total);
  if (r != JDR_OK) Serial.printf("[ART] decode err %d\n", (int)r);

  free(jpg);
}

void drawTrackInfo(bool force = false) {
  tft.setTextDatum(TL_DATUM);

  if (force || curr.title != prev.title) {
    tft.setTextColor(C_WHITE, C_BG);
    tft.setTextSize(2);
    tft.drawString(fitString(curr.title.length() ? curr.title : "---", 3, INFO_W), INFO_X, INFO_Y + 10);
  }
  if (force || curr.artist != prev.artist) {
    tft.setTextColor(C_GRAY, C_BG);
    tft.setTextSize(2);
    tft.drawString(fitString(curr.artist.length() ? curr.artist : "No device active", 3, INFO_W), INFO_X, INFO_Y + 50);
  }
  if (force || curr.album != prev.album) {
    tft.setTextColor(C_MUTED, C_BG);
    tft.setTextSize(2);
    tft.drawString(fitString(curr.album, 2, INFO_W), INFO_X, INFO_Y + 90);
  }
  if (force || curr.saved != prev.saved) {
    if (curr.saved) {
      tft.setTextColor(C_GREEN, C_BG);
      tft.setTextSize(2);
      tft.drawString("+ LIKED", INFO_X, INFO_Y + 150);
    }
  }
}

void drawProgressBar() {
  tft.fillRoundRect(PB_X, PB_Y, PB_W, PB_H, 3, C_BORDER);
  if (curr.duration_ms > 0) {
    int fill = constrain((int)((float)curr.progress_ms / curr.duration_ms * PB_W), 0, PB_W);
    if (fill > 0) tft.fillRoundRect(PB_X, PB_Y, fill, PB_H, 3, C_GREEN);
  }
  tft.fillRect(PB_X, TIME_Y + 4, 50, 10, C_BG);
  tft.fillRect(PB_X + PB_W - 50, TIME_Y + 4, 50, 10, C_BG);
  tft.setTextColor(C_GRAY, C_BG);
  tft.setTextSize(2);
  char buf[8];
  int s = curr.progress_ms / 1000;
  snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(buf, PB_X, TIME_Y + 10);
  s = curr.duration_ms / 1000;
  snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(buf, PB_X + PB_W, TIME_Y + 10);
}

void drawPlayPauseBtn(bool playing) {
  tft.fillCircle(BTN_PP_X, CTRL_Y, BTN_R_LG, C_GREEN);
  if (playing) {
    tft.fillRect(BTN_PP_X - 10, CTRL_Y - 11, 7, 22, TFT_BLACK);
    tft.fillRect(BTN_PP_X + 3,  CTRL_Y - 11, 7, 22, TFT_BLACK);
  } else {
    for (int i = 0; i < 16; i++) {
      int h = 2 * (16 - i);
      tft.drawFastVLine(BTN_PP_X - 7 + i, CTRL_Y - h / 2, h, TFT_BLACK);
    }
  }
}

void drawSkipBtn(int cx, bool isPrev) {
  tft.fillCircle(cx, CTRL_Y, BTN_R_SM, C_CARD);
  tft.drawCircle(cx, CTRL_Y, BTN_R_SM, C_BORDER);
  int dir = isPrev ? -1 : 1;
  for (int i = 0; i < 13; i++) {
    int h = 2 * (13 - i);
    tft.drawFastVLine(cx + dir * (-5 + i), CTRL_Y - h / 2, h, C_WHITE);
  }
  tft.fillRect(cx + dir * 7, CTRL_Y - 11, 4, 22, C_WHITE);
}

void drawControls(bool force = false) {
  if (force) {
    tft.fillRect(0, CTRL_Y - BTN_R_LG - 2, 480, BTN_R_LG * 2 + 4, C_BG);
    drawSkipBtn(BTN_PREV_X, true);
    drawSkipBtn(BTN_NEXT_X, false);
  }
  if (force || curr.playing != prev.playing) drawPlayPauseBtn(curr.playing);
}

void fullRedraw() {
  tft.fillScreen(C_BG);
  drawAlbumArt(currAlbumArtUrl);
  drawTrackInfo(true);
  drawProgressBar();
  drawControls(true);
  needsFullRedraw = false;
}

bool httpsGet(const String &url, const String &token, String &out, int *st = nullptr) {
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, url);
  if (token.length()) h.addHeader("Authorization", "Bearer " + token);
  h.setTimeout(8000);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = h.GET();
  if (st) *st = code;
  out = h.getString();
  h.end();
  return (code == 200 || code == 201);
}

bool httpsPost(const String &url, const String &ct, const String &body,
               const String &token, String &out, int *st = nullptr) {
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, url);
  h.addHeader("Content-Type", ct);
  if (token.length()) h.addHeader("Authorization", "Bearer " + token);
  h.setTimeout(10000);
  int code = h.POST((uint8_t *)body.c_str(), body.length());
  if (st) *st = code;
  out = h.getString();
  h.end();
  return (code >= 200 && code < 300);
}

bool httpsPut(const String &url, const String &token) {
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, url);
  h.addHeader("Content-Type", "application/json");
  h.addHeader("Authorization", "Bearer " + token);
  h.addHeader("Content-Length", "0");
  h.setTimeout(8000);
  int code = h.PUT("");
  h.end();
  return (code >= 200 && code < 300);
}

String getRedirectUri() {
#ifdef SPOTIFY_REDIRECT_URI_FIXED
  return String(SPOTIFY_REDIRECT_URI_FIXED);
#else
  return String("http://") + WiFi.localIP().toString() + "/callback";
#endif
}

String buildAuthUrl() {
  String scopes = SPOTIFY_SCOPES;
  scopes.replace(" ", "%20");
  return String(SPOTIFY_AUTH_BASE)
    + "?response_type=code"
    + "&client_id=" + SPOTIFY_CLIENT_ID
    + "&scope=" + scopes
    + "&redirect_uri=" + getRedirectUri();
}

bool exchangeCode(const String &code, String &acc, String &ref, int &exp) {
  String creds = String(SPOTIFY_CLIENT_ID) + ":" + SPOTIFY_CLIENT_SECRET;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, SPOTIFY_TOKEN_URL);
  h.addHeader("Authorization", "Basic " + base64::encode(creds));
  h.addHeader("Content-Type", "application/x-www-form-urlencoded");
  h.setTimeout(10000);
  String body = "grant_type=authorization_code&code=" + code + "&redirect_uri=" + getRedirectUri();
  int rc = h.POST((uint8_t *)body.c_str(), body.length());
  String resp = h.getString(); h.end();
  if (rc != 200) { Serial.printf("[OAuth] exchange %d: %s\n", rc, resp.c_str()); return false; }
  JsonDocument doc; deserializeJson(doc, resp);
  acc = doc["access_token"].as<String>();
  ref = doc["refresh_token"].as<String>();
  exp = doc["expires_in"] | 3600;
  return acc.length() > 0;
}

bool refreshAccessToken(const String &rt, String &newAcc, int &exp) {
  String creds = String(SPOTIFY_CLIENT_ID) + ":" + SPOTIFY_CLIENT_SECRET;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, SPOTIFY_TOKEN_URL);
  h.addHeader("Authorization", "Basic " + base64::encode(creds));
  h.addHeader("Content-Type", "application/x-www-form-urlencoded");
  h.setTimeout(10000);
  String body = "grant_type=refresh_token&refresh_token=" + rt;
  int code = h.POST((uint8_t *)body.c_str(), body.length());
  String resp = h.getString(); h.end();
  if (code != 200) { Serial.printf("[OAuth] refresh %d\n", code); return false; }
  JsonDocument doc; deserializeJson(doc, resp);
  newAcc = doc["access_token"].as<String>();
  exp = doc["expires_in"] | 3600;
  String newRt = doc["refresh_token"] | "";
  if (newRt.length()) saveRefreshToken(newRt);
  return newAcc.length() > 0;
}

bool ensureToken() {
  if (g_accessToken.length() && millis() < g_tokenExpiresAt - 60000) return true;
  String rt = loadRefreshToken();
  if (!rt.length()) return false;
  String newAcc; int exp;
  if (!refreshAccessToken(rt, newAcc, exp)) return false;
  g_accessToken    = newAcc;
  g_tokenExpiresAt = millis() + (uint32_t)exp * 1000;
  return true;
}

void spotifyPlay()  { httpsPut(String(SPOTIFY_API_BASE) + "/me/player/play",  g_accessToken); }
void spotifyPause() { httpsPut(String(SPOTIFY_API_BASE) + "/me/player/pause", g_accessToken); }
void spotifySeek(int ms) {
  httpsPut(String(SPOTIFY_API_BASE) + "/me/player/seek?position_ms=" + ms, g_accessToken);
}

void spotifyPost(const String &path) {
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, String(SPOTIFY_API_BASE) + path);
  h.addHeader("Authorization", "Bearer " + g_accessToken);
  h.setTimeout(8000);
  h.POST(""); h.end();
}
void spotifyNext() { spotifyPost("/me/player/next"); }
void spotifyPrev() { spotifyPost("/me/player/previous"); }

void spotifyToggleSave(const String &id, bool save) {
  String url = String(SPOTIFY_API_BASE) + "/me/tracks?ids=" + id;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, url);
  h.addHeader("Authorization", "Bearer " + g_accessToken);
  h.addHeader("Content-Type", "application/json");
  h.addHeader("Content-Length", "0");
  h.setTimeout(8000);
  if (save) h.PUT(""); else h.sendRequest("DELETE");
  h.end();
}

bool pollPlayback() {
  String resp; int status = 0;
  bool ok = httpsGet(String(SPOTIFY_API_BASE) + "/me/player", g_accessToken, resp, &status);
  if (!ok) { curr.active = (status != 204); return true; }

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;

  curr.active      = true;
  curr.playing     = doc["is_playing"] | false;
  curr.progress_ms = doc["progress_ms"] | 0;

  JsonObject item = doc["item"];
  if (item.isNull()) { curr.active = false; return true; }

  String newId  = item["id"].as<String>();
  bool changed  = (newId != curr.id);
  curr.id       = newId;
  curr.duration_ms = item["duration_ms"] | 0;
  curr.title    = item["name"].as<String>();
  curr.album    = item["album"]["name"].as<String>();

  JsonArray imgs = item["album"]["images"].as<JsonArray>();
  if (!imgs.isNull() && imgs.size() > 0) {
    String best = imgs[0]["url"] | "";
    for (JsonObject img : imgs) {
      int w = img["width"] | 0;
      if (w >= ART_W && w < (int)(imgs[0]["width"] | 9999)) best = img["url"] | best;
    }
    if (best.length()) currAlbumArtUrl = best;
  }

  String artists;
  for (JsonObject a : item["artists"].as<JsonArray>()) {
    if (artists.length()) artists += ", ";
    artists += a["name"].as<String>();
  }
  curr.artist = artists;

  if (changed && curr.id.length()) {
    String sr;
    if (httpsGet(String(SPOTIFY_API_BASE) + "/me/tracks/contains?ids=" + curr.id, g_accessToken, sr)) {
      JsonDocument sd; deserializeJson(sd, sr);
      curr.saved = sd[0] | false;
    }
  } else {
    curr.saved = prev.saved;
  }
  return true;
}

WebServer        portalServer(80);
volatile bool    g_codeReceived = false;
String           g_authCode;

void portalHandleRoot() {
  String authUrl = buildAuthUrl();
  String ip = WiFi.localIP().toString();
  String html =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#000;color:#fff;display:flex;flex-direction:column;"
    "align-items:center;justify-content:center;min-height:100vh;margin:0;padding:20px;}"
    "h2{color:#1DB954;}a{display:inline-block;background:#1DB954;color:#000;padding:14px 28px;"
    "border-radius:28px;text-decoration:none;font-weight:700;margin-top:14px;}"
    "code{color:#1DB954;}</style></head><body>"
    "<h2>Spotify Setup</h2>"
    "<p>ESP32 IP: <code>" + ip + "</code></p>"
    "<a href='" + authUrl + "'>Authorize with Spotify</a>"
    "<p style='font-size:12px'>Redirect URI: <code>" + getRedirectUri() + "</code></p>"
    "</body></html>";
  portalServer.send(200, "text/html", html);
}

void portalHandleCallback() {
  if (portalServer.hasArg("code")) {
    g_authCode      = portalServer.arg("code");
    g_codeReceived  = true;
    portalServer.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#000;color:#1DB954;"
      "display:flex;align-items:center;justify-content:center;min-height:100vh;text-align:center'>"
      "<div><h2>Done!</h2><p style='color:#aaa'>Close this page.</p></div></body></html>");
  } else {
    portalServer.send(400, "text/plain", "Missing code");
  }
}

bool runLanAuthFlow() {
  portalServer.on("/",         HTTP_GET, portalHandleRoot);
  portalServer.on("/callback", HTTP_GET, portalHandleCallback);
  portalServer.onNotFound([]() { portalServer.send(404, "text/plain", "Not found"); });
  portalServer.begin();

  tft.fillScreen(C_BG);
  drawHeader("AUTH");
  tft.setTextColor(C_WHITE, C_BG); tft.setTextDatum(TC_DATUM); tft.setTextSize(2);
  tft.drawString("Open on your phone:", 240, 80);
  tft.setTextColor(C_GREEN, C_BG);
  tft.drawString(WiFi.localIP().toString(), 240, 110);
  tft.setTextColor(C_GRAY, C_BG); tft.setTextSize(1);
  tft.drawString("Same WiFi network required", 240, 145);
  tft.drawString("Then tap Authorize", 240, 160);

  uint32_t t0 = millis();
  while (!g_codeReceived) {
    portalServer.handleClient();
    if (millis() - t0 > 180000) {
      drawStatus("Auth timeout", C_RED);
      portalServer.stop(); return false;
    }
    delay(10);
  }
  portalServer.stop();
  drawStatus("Exchanging tokens...", C_GREEN);

  String acc, ref; int exp = 0;
  if (!exchangeCode(g_authCode, acc, ref, exp)) {
    drawStatus("Auth failed", C_RED); delay(5000); return false;
  }
  saveRefreshToken(ref);
  g_accessToken    = acc;
  g_tokenExpiresAt = millis() + (uint32_t)exp * 1000;
  drawStatus("Authorized!", C_GREEN);
  delay(800);
  return true;
}

#define TOUCH_X_MIN  300
#define TOUCH_X_MAX  3800
#define TOUCH_Y_MIN  300
#define TOUCH_Y_MAX  3800

bool inCircle(int tx, int ty, int cx, int cy, int r) {
  int dx = tx - cx, dy = ty - cy;
  return dx*dx + dy*dy <= r*r;
}

void mapTouch(int rawX, int rawY, int &sx, int &sy) {
  sx = map(rawX, TOUCH_X_MIN, TOUCH_X_MAX, 0, 479);
  sy = map(rawY, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 319);
  sx = constrain(sx, 0, 479);
  sy = constrain(sy, 0, 319);
}

void handleTouch() {
  if (!touch.touched()) return;
  uint32_t now = millis();
  if (now - lastTouchMs < 350) return;
  lastTouchMs = now;

  TS_Point p = touch.getPoint();
  int sx, sy;
  mapTouch(p.x, p.y, sx, sy);
  Serial.printf("[Touch] raw(%d,%d) -> screen(%d,%d)\n", p.x, p.y, sx, sy);

  // if (!ensureToken()) return;

  if (inCircle(sx, sy, BTN_PREV_X, CTRL_Y, BTN_R_SM)) {
    spotifyPrev(); delay(300); lastPollMs = 0; return;
  }
  if (inCircle(sx, sy, BTN_PP_X, CTRL_Y, BTN_R_LG)) {
    if (curr.playing) spotifyPause(); else spotifyPlay();
    curr.playing = !curr.playing;
    drawPlayPauseBtn(curr.playing); return;
  }
  if (inCircle(sx, sy, BTN_NEXT_X, CTRL_Y, BTN_R_SM)) {
    spotifyNext(); delay(300); lastPollMs = 0; return;
  }
}

bool connectHomeWifi(uint32_t timeout = 20000) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);
  Serial.printf("[WiFi] Connecting to '%s'...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout) {
    delay(300);
    Serial.printf("[WiFi] status=%d\n", WiFi.status());
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  tft.begin();
  tft.setRotation(1);

#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
#endif

  TJpgDec.setCallback(tft_output);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);

  SPI.begin(14, 33, 13, TOUCH_CS);
  touch.begin();
  touch.setRotation(1);

  tft.fillScreen(C_BG);
  drawHeader();
  tft.setTextColor(C_GREEN, C_BG); tft.setTextDatum(MC_DATUM); tft.setTextSize(3);
  tft.drawString("SPOTIFY", 240, 140);
  tft.setTextColor(C_GRAY, C_BG); tft.setTextSize(2);
  tft.drawString("Starting up...", 240, 175);

  if (!connectHomeWifi()) {
    drawStatus("WiFi failed (2.4GHz?)", C_RED);
    delay(4000); ESP.restart(); return;
  }
  delay(500);
  drawStatus("WiFi connected!");

  String rt = loadRefreshToken();
  if (!rt.length()) {
    if (!runLanAuthFlow()) { delay(2000); ESP.restart(); return; }
  }

  if (!ensureToken()) {
    drawStatus("Token error -> re-auth", C_RED);
    clearRefreshToken(); delay(2000); ESP.restart(); return;
  }

  needsFullRedraw = true;
}

void loop() {
  uint32_t now = millis();

  handleTouch();

  if (curr.active && curr.playing && now - lastTickMs >= 500) {
    lastTickMs = now;
    curr.progress_ms = min(curr.progress_ms + 500, curr.duration_ms);
    drawProgressBar();
  }

  if (now - lastPollMs >= 3000 || needsFullRedraw) {
    lastPollMs = now;

    if (WiFi.status() != WL_CONNECTED) {
      drawStatus("WiFi lost...", C_RED);
      WiFi.reconnect(); return;
    }
    if (!ensureToken()) { drawStatus("Token error", C_RED); return; }

    Track before = curr;
    if (pollPlayback()) {
      bool trackChanged  = (curr.id != before.id);
      bool activeChanged = (curr.active != prev.active);

      if (trackChanged) {
        g_lastArtUrl = "";
      }

      if (needsFullRedraw || trackChanged || activeChanged) {
        fullRedraw();
      } else {
        drawTrackInfo(false);
        drawProgressBar();
        drawControls(false);
      }
      prev = before;
    }
  }

  delay(20);
}
