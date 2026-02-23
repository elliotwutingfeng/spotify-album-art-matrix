#include <WiFiS3.h>              // Built-in
#include <ArduinoHttpClient.h>   // 0.6.1
#include <base64.hpp>            // 1.3.0
#include <Adafruit_GFX.h>        // 1.12.4
#include <Adafruit_NeoMatrix.h>  // 1.3.3
#include <Adafruit_NeoPixel.h>   // 1.15.4
#include <TJpg_Decoder.h>        // 1.1.0

// Patches from <https://github.com/MikePeak/json-streaming-parser2/tree/fix/endobject-off-by-one>
#include "src/JsonStreamingParser2.h"  // json-streaming-parser2 by mrcodetastic <https://github.com/mrcodetastic/json-streaming-parser2>

#include "bsod.h"
#include "panda.h"

#include "arduino_secrets.h"
#define ACCESS_TOKEN_BODY "grant_type=refresh_token&refresh_token=" REFRESH_TOKEN


#define SCREEN_UPDATE_INTERVAL_MS 1000UL  // Increase this value if you are hitting Spotify API rate limits.
#define HTTP_TIMEOUT_MS 7000UL
#define HTTP_CHUNK_SIZE_BYTES 1024
#define WS2812B_DATA_PIN 6

static Adafruit_NeoMatrix matrix = Adafruit_NeoMatrix(32, 8, 1, 4, WS2812B_DATA_PIN,
                                                      NEO_TILE_TOP + NEO_TILE_RIGHT + NEO_TILE_ROWS + NEO_TILE_ZIGZAG + NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG,
                                                      NEO_GRB + NEO_KHZ800);

static WiFiSSLClient wifiSSLClient;

static char cachedSpotifyAccessToken[384];
static char cachedSpotifyCurrentAlbumId[96];
static char cachedSpotifyAlbumImageUrl[96];

enum HttpMethod { GET,
                  POST };

enum Status { NEW_CURRENT_ALBUM,
              EXISTING_CURRENT_ALBUM,
              NO_CURRENT_ALBUM,
              CANNOT_GET_ACCESS_TOKEN,
              ALBUM_ID_PARSE_FAILED,
              ALBUM_IMAGE_URL_PARSE_FAILED,
              UNEXPECTED_ERROR
};

// Non-blocking alternative to standard delay()
static void millisDelay(unsigned long ms) {
  const unsigned long start = millis();
  while (millis() - start < ms) {}
}

// Send HTTP request with optional auth header and body
static int sendRequest(HttpClient &client, HttpMethod httpMethod, const char *path,
                       const char *authHeader = nullptr, const char *body = nullptr) {
  client.beginRequest();
  switch (httpMethod) {
    case GET:
      client.get(path);
      break;
    case POST:
      client.post(path);
      break;
  }
  if (authHeader) { client.sendHeader("Authorization", authHeader); }
  if (body) {
    client.sendHeader("Content-Type", "application/x-www-form-urlencoded");
    client.sendHeader("Content-Length", strlen(body));
    client.print(body);
  }
  client.endRequest();

  int status = client.responseStatusCode();
  client.skipResponseHeaders();
  return status;
}

// Retrieve Spotify access token either from cache or by requesting a new one from Spotify Web API
static bool getAccessToken() {
  // Reuse cached access token if available
  if (cachedSpotifyAccessToken[0] != '\0') { return true; }

  static const char authRaw[] = CLIENT_ID ":" CLIENT_SECRET;
  char authHeader[6 + encode_base64_length(sizeof(authRaw) - 1)];
  memcpy(authHeader, "Basic ", 6);
  encode_base64((uint8_t *)authRaw, strlen(authRaw), (uint8_t *)authHeader + 6);

  HttpClient httpClient(wifiSSLClient, "accounts.spotify.com", 443);
  int status = sendRequest(httpClient, POST, "/api/token", authHeader, ACCESS_TOKEN_BODY);

  if (status != 200) {
    Serial.print(F("Error | getAccessToken HTTP status code"));
    Serial.println(status);
    httpClient.stop();
    return false;
  }

  // Stream-search JSON for top-most "access_token":"<value>"
  enum { HUNT,
         IN_VALUE } state = HUNT;
  const char accessTokenHeader[] = "\"access_token\":\"";
  size_t nLen = strlen(accessTokenHeader), nPos = 0, vPos = 0;

  unsigned long start = millis();
  uint8_t chunk[HTTP_CHUNK_SIZE_BYTES];

  bool done = false;
  char c = '\0';

  while (!done && (httpClient.connected() || httpClient.available()) && millis() - start < HTTP_TIMEOUT_MS) {
    int n = 0;
    if (httpClient.available()) {
      n = httpClient.read(chunk, sizeof(chunk));
    }
    if (n <= 0) { continue; }
    for (int i = 0; i < n && !done; i++) {
      uint8_t b = chunk[i];
      if (b > 127) { continue; }  // Skip non-ASCII bytes
      c = (char)b;
      switch (state) {
        case HUNT:
          if (c == accessTokenHeader[nPos]) {
            if (++nPos == nLen) state = IN_VALUE;
          } else {
            // Mismatch: reset and recheck current character from the beginning
            nPos = 0;
            if (c == accessTokenHeader[nPos]) {
              nPos = 1;
            }
          }
          break;
        case IN_VALUE:
          if (c == '"' || vPos >= sizeof(cachedSpotifyAccessToken) - 1) {
            done = true;
            break;
          }
          cachedSpotifyAccessToken[vPos++] = c;
          break;
      }
    }
  }
  cachedSpotifyAccessToken[vPos] = '\0';
  httpClient.stop();

  if (!vPos || c != '"') {
    Serial.println(F("Error | getAccessToken parse failed"));
    return false;
  }
  return true;
}

// Extract album ID and smallest-width album image URL from
// the Spotify currently-playing JSON response.
// Targeted paths: "item.album.id" and "item.album.images[*].{url,width}"
class SpotifyAlbumHandler : public JsonHandler {
public:
  char maybeAlbumId[sizeof(cachedSpotifyCurrentAlbumId)];
  char bestUrl[sizeof(cachedSpotifyAlbumImageUrl)];
  int bestWidth;
  bool done;

private:
  bool inImages;
  char curUrl[sizeof(cachedSpotifyAlbumImageUrl)];
  int curWidth;

  // Return true if path is exactly item -> album -> ${key}
  static bool isItemAlbumKey(ElementPath &path, const char *key) {
    int count = path.getCount();
    return count == 3 && strcmp(path.getKey(0), "item") == 0 && strcmp(path.getKey(1), "album") == 0 && strcmp(path.getKey(2), key) == 0;
  }

public:
  SpotifyAlbumHandler() {
    maybeAlbumId[0] = '\0';
    bestUrl[0] = '\0';
    bestWidth = 0x7FFFFFFF;
    done = false;
    inImages = false;
    curUrl[0] = '\0';
    curWidth = -1;
  }

  void startDocument() {}
  void endDocument() {}
  void whitespace(char c) {}

  void startArray(ElementPath path) {
    if (isItemAlbumKey(path, "images")) {
      inImages = true;
    }
  }

  void endArray(ElementPath path) {
    if (inImages && isItemAlbumKey(path, "images")) {
      inImages = false;
      done = true;
    }
  }

  void startObject(ElementPath path) {
    if (inImages) {
      curUrl[0] = '\0';
      curWidth = -1;
    }
  }

  void endObject(ElementPath path) {
    if (inImages && curUrl[0] != '\0' && curWidth != -1 && curWidth < bestWidth) {
      bestWidth = curWidth;
      memcpy(bestUrl, curUrl, strlen(curUrl) + 1);
    }
  }

  void value(ElementPath path, ElementValue val) {
    const char *key = path.getKey();
    if (!key || key[0] == '\0') { return; }

    if (inImages) {
      if (strcmp(key, "url") == 0 && val.isString()) {
        size_t valLength = strlen(val.getString());
        if (valLength >= sizeof(curUrl)) { valLength = sizeof(curUrl) - 1; }
        memcpy(curUrl, val.getString(), valLength + 1);
      } else if (strcmp(key, "width") == 0) {
        if (val.isInt()) {
          curWidth = (int)val.getInt();
        } else if (val.isFloat()) {
          curWidth = (int)val.getFloat();
        }
      }
      return;
    }
    if (isItemAlbumKey(path, "id") && val.isString()) {
      size_t valLength = strlen(val.getString());
      if (valLength >= sizeof(maybeAlbumId)) { valLength = sizeof(maybeAlbumId) - 1; }
      memcpy(maybeAlbumId, val.getString(), valLength + 1);
    }
  }
};

// Update cachedSpotifyAlbumImageUrl and cachedSpotifyCurrentAlbumId if a new current album is found. Return the album update status (i.e. whether there is a new current album, the same current album, or no current album, or error status if any step fails)
static Status handleCurrentlyPlayingResponse(HttpClient &httpClient) {
  SpotifyAlbumHandler handler;
  JsonStreamingParser parser;
  parser.setHandler(&handler);

  unsigned long start = millis();
  uint8_t chunk[HTTP_CHUNK_SIZE_BYTES];

  while (!handler.done && (httpClient.connected() || httpClient.available()) && millis() - start < HTTP_TIMEOUT_MS) {
    int n = 0;
    if (httpClient.available()) {
      n = httpClient.read(chunk, sizeof(chunk));
    }
    if (n <= 0) { continue; }
    for (int i = 0; i < n && !handler.done; i++) {
      uint8_t b = chunk[i];
      if (b > 127) { continue; }  // Our JSON parser cannot handle non-ASCII bytes
      parser.parse((char)b);
    }
  }

  httpClient.stop();

  if (handler.maybeAlbumId[0] == '\0') {
    // Current media has no album id
    // This is usually the case for podcasts
    // Some songs may not have album art
    Serial.println(F("Album ID: parse failed"));
    return ALBUM_ID_PARSE_FAILED;
  }

  if (strcmp(handler.maybeAlbumId, cachedSpotifyCurrentAlbumId) == 0) {
    Serial.print(F("Existing Album ID: "));
    Serial.println(handler.maybeAlbumId);
    return EXISTING_CURRENT_ALBUM;
  }

  if (handler.bestUrl[0] == '\0') {
    Serial.println(F("Album Image URL: parse failed"));
    return ALBUM_IMAGE_URL_PARSE_FAILED;
  }

  memcpy(cachedSpotifyAlbumImageUrl, handler.bestUrl, strlen(handler.bestUrl) + 1);
  memcpy(cachedSpotifyCurrentAlbumId, handler.maybeAlbumId, strlen(handler.maybeAlbumId) + 1);

  return NEW_CURRENT_ALBUM;
}

// Retrieve currently playing album details from Spotify Web API and save to cache with maximum 2 attempts (i.e. 1 retry if the first attempt fails with 401, which may indicate an expired cached access token). Return the album update status (i.e. whether there is a new current album, the same current album, or no current album, or error status if any step fails)
static Status saveCurrentlyPlayingDetails() {
  const int maxTries = 2;

  for (int attempt = 0; attempt < maxTries; attempt++) {
    HttpClient httpClient(wifiSSLClient, "api.spotify.com", 443);
    if (!getAccessToken()) {
      httpClient.stop();
      return CANNOT_GET_ACCESS_TOKEN;
    }

    char authHeader[7 + sizeof(cachedSpotifyAccessToken)];
    memcpy(authHeader, "Bearer ", 7);
    memcpy(authHeader + 7, cachedSpotifyAccessToken, strlen(cachedSpotifyAccessToken) + 1);
    int status = sendRequest(httpClient, GET, "/v1/me/player/currently-playing", authHeader);

    if (status == 200) {
      // Something is playing.
      return handleCurrentlyPlayingResponse(httpClient);
    }

    httpClient.stop();

    if (status == 204) {
      Serial.println(F("No current album"));
      return NO_CURRENT_ALBUM;
    }
    if (status == 401) {
      if (attempt == maxTries - 1) {
        return CANNOT_GET_ACCESS_TOKEN;
      }
      // Clear cached access token and retry with a fresh one.
      cachedSpotifyAccessToken[0] = '\0';
      continue;
    }

    // Unexpected error type; do not retry.
    Serial.print(F("Error | saveCurrentlyPlayingDetails HTTP status code"));
    Serial.println(status);
    return UNEXPECTED_ERROR;
  }
}

// NOTE: JpegHttpContext, jpegHttpInput and jpegHttpOutput stream JPEG data directly from HTTP and resize it to 32x32 on the fly, for display on our LED matrix.

// Raw TJpeg streaming callbacks (bypass TJpgDec high-level API to avoid buffering)
struct JpegHttpContext {
  HttpClient *client;
  unsigned long deadline;
  uint16_t srcW;  // decoded width  (jdec.width  >> scale)
  uint16_t srcH;  // decoded height (jdec.height >> scale)
};

// TJpgDec input callback that reads JPEG data from HTTP response in a streaming manner, with timeout. If buf is nullptr, skip len bytes by draining. Return number of bytes read or skipped
static size_t jpegHttpInput(JDEC *jd, uint8_t *buf, size_t len) {
  JpegHttpContext *ctx = (JpegHttpContext *)jd->device;
  if (millis() >= ctx->deadline) { return 0; }  // Exit early on timeout.

  // Skip Mode
  if (buf == nullptr) {
    // Skip len bytes by draining
    size_t skipped = 0;
    while (skipped < len && (ctx->client->connected() || ctx->client->available())) {
      if (millis() >= ctx->deadline) { break; }  // Exit early on timeout.
      if (ctx->client->available() && ctx->client->read() >= 0) { skipped++; }
    }
    return skipped;
  }

  // Read Mode
  size_t total = 0;
  while (total < len && (ctx->client->connected() || ctx->client->available())) {
    if (millis() >= ctx->deadline) { break; }  // Exit early on timeout.
    int n = 0;
    if (ctx->client->available()) { n = ctx->client->read(buf + total, len - total); }  // Handle partial reads by looping until we read the requested len or hit timeout.
    if (n > 0) { total += n; }
  }
  return total;
}

// TJpgDec output callback that maps decoded pixels to the 32x32 matrix with nearest-neighbor scaling.
static int jpegHttpOutput(JDEC *jd, void *bitmap, JRECT *rect) {
  JpegHttpContext *ctx = (JpegHttpContext *)jd->device;
  uint16_t *pixels = (uint16_t *)bitmap;  // Each pixel is 16-bit RGB565.
  uint16_t w = rect->right - rect->left + 1;
  for (uint16_t row = 0; row <= rect->bottom - rect->top; row++) {
    for (uint16_t col = 0; col < w; col++) {
      // Nearest-neighbor map from decoded space -> 32x32
      int16_t dstX = (int32_t)(rect->left + col) * 32 / ctx->srcW;
      int16_t dstY = (int32_t)(rect->top + row) * 32 / ctx->srcH;
      matrix.drawPixel(dstX, dstY, pixels[row * w + col]);
    }
  }
  return 1;  // Return 1 to tell decoder to continue decoding for next output block.
}

// Fetch album art from cachedSpotifyAlbumImageUrl and display on the matrix. Return true if successful, false if any step fails (e.g. HTTP error, JPEG decode error, or timeout)
static bool fetchAndDisplayAlbumArt() {
  // Parse "https://<host><path>" from cachedSpotifyAlbumImageUrl.
  // cachedSpotifyAlbumImageUrl is likely "https://i.scdn.co/<path>"
  const char *spotifyImageUrlHostStart = cachedSpotifyAlbumImageUrl + 8;  // skip "https://"
  const char *path = strchr(spotifyImageUrlHostStart, '/');
  if (!path) {
    Serial.println(F("Malformed image URL: no path"));
    return false;
  }
  size_t spotifyImageUrlHostLen = path - spotifyImageUrlHostStart;
  if (spotifyImageUrlHostLen >= sizeof(cachedSpotifyAlbumImageUrl)) {
    Serial.println(F("Malformed image URL: host too long"));
    return false;
  }
  char spotifyImageUrlHost[sizeof(cachedSpotifyAlbumImageUrl)];
  memcpy(spotifyImageUrlHost, spotifyImageUrlHostStart, spotifyImageUrlHostLen);
  spotifyImageUrlHost[spotifyImageUrlHostLen] = '\0';

  HttpClient httpClient(wifiSSLClient, spotifyImageUrlHost, 443);
  int status = sendRequest(httpClient, GET, path);

  if (status != 200) {
    httpClient.stop();
    Serial.print(F("Error | fetchAndDisplayAlbumArt HTTP status code"));
    Serial.println(status);
    return false;
  }

  Serial.println(F("==="));
  Serial.print(F("New Album ID: "));
  Serial.println(cachedSpotifyCurrentAlbumId);
  Serial.print(F("Album Image URL: "));
  Serial.println(cachedSpotifyAlbumImageUrl);
  Serial.println(F("==="));

  static uint8_t jpegWorkPool[TJPGD_WORKSPACE_SIZE];
  JDEC jdec;
  JpegHttpContext ctx = { &httpClient, millis() + HTTP_TIMEOUT_MS };
  JRESULT res = jd_prepare(&jdec, jpegHttpInput, jpegWorkPool, sizeof(jpegWorkPool), &ctx);
  if (res == JDR_OK) {
    // jd_decomp scale: 0=1/1, 1=1/2, 2=1/4, 3=1/8 (capped at 3 by TJpeg)
    // Pick the smallest scale (biggest reduction) where decoded size still >= 32
    // in both axes, so we don't upscale. Then nearest-neighbor maps to 32x32.
    uint8_t scale = 0;
    while (scale < 3 && ((jdec.width >> (scale + 1)) >= 32) && ((jdec.height >> (scale + 1)) >= 32)) {
      scale++;
    }
    ctx.srcW = jdec.width >> scale;
    ctx.srcH = jdec.height >> scale;

    Serial.print(F("JPEG original dimensions: "));
    Serial.print(jdec.width);
    Serial.print(F(" x "));
    Serial.println(jdec.height);

    Serial.print(F("JPEG decoded dimensions: "));
    Serial.print(ctx.srcW);
    Serial.print(F(" x "));
    Serial.println(ctx.srcH);

    jd_decomp(&jdec, jpegHttpOutput, scale);
  } else {
    Serial.print(F("Error | jd_prepare: "));
    Serial.println((int)res);
  }
  httpClient.stop();

  return true;
}

// Update screen based on currently playing album details.
static void updateScreen() {
  Serial.println(F("Updating screen..."));
  static Status previousStatus = (Status)(-1);
  Status status = saveCurrentlyPlayingDetails();
  if (status != NEW_CURRENT_ALBUM && status == previousStatus) { return; }
  previousStatus = status;

  switch (status) {
    case NEW_CURRENT_ALBUM:
      if (!fetchAndDisplayAlbumArt()) {
        // Album art exists, but download failed. Show BSOD 😭 to reflect the tragedy.
        for (uint16_t y = 0; y < 32; y++) {
          for (uint16_t x = 0; x < 32; x++) {
            matrix.drawPixel(x, y, pgm_read_word(&bsod[y * 32 + x]));
          }
        }
      }
      break;
    case EXISTING_CURRENT_ALBUM:
      return;
    case NO_CURRENT_ALBUM:
      // Nothing playing. Baby panda 妹猪 (mèi zhū) 🐼 is sleeping peacefully.
      for (uint16_t y = 0; y < 32; y++) {
        for (uint16_t x = 0; x < 32; x++) {
          matrix.drawPixel(x, y, pgm_read_word(&panda[y * 32 + x]));
        }
      }
      break;
    case CANNOT_GET_ACCESS_TOKEN:
      {
        // Access denied. Draw a big magenta cross ❌.
        const uint16_t magenta = 0xD95F;
        const uint16_t black = 0x0000;
        matrix.fillScreen(black);
        const int thickness = 6;
        for (int offset = -thickness / 2; offset <= thickness / 2; offset++) {
          matrix.drawLine(0, 0 + offset, 31, 31 + offset, magenta);  // Diagonal '\'
          matrix.drawLine(31, 0 + offset, 0, 31 + offset, magenta);  // Diagonal '/'
        }
        break;
      }
    case ALBUM_ID_PARSE_FAILED:
      {
        // No album ID; some entries have no albums, like podcasts. Draw a blue question mark ❓.
        const uint16_t blue = 0x327F;
        const uint16_t black = 0x0000;
        matrix.fillScreen(black);
        matrix.setCursor(6, 2);
        matrix.setTextColor(blue);
        matrix.setTextSize(4);
        matrix.print(F("?"));
        break;
      }
    case ALBUM_IMAGE_URL_PARSE_FAILED:
      {
        // Album exists, but no album art. Draw a yellow question mark ❓.
        const uint16_t yellow = 0xFD00;
        const uint16_t black = 0x0000;
        matrix.fillScreen(black);
        matrix.setCursor(6, 2);
        matrix.setTextColor(yellow);
        matrix.setTextSize(4);
        matrix.print(F("?"));
        break;
      }
    case UNEXPECTED_ERROR:
      {
        // 🇸🇬 flag; a dose of patriotism for when all else fails.

        // colors
        const uint16_t flagRed = 0xE926;
        const uint16_t flagWhite = 0xFFFF;

        // background
        matrix.fillRect(0, 0, 32, 16, flagRed);
        matrix.fillRect(0, 16, 32, 16, flagWhite);

        // crescent
        matrix.fillRect(2, 7, 3, 3, flagWhite);
        matrix.drawFastHLine(3, 10, 2, flagWhite);
        matrix.drawFastHLine(3, 6, 2, flagWhite);
        matrix.drawFastHLine(3, 11, 3, flagWhite);
        matrix.drawFastHLine(3, 5, 3, flagWhite);
        matrix.drawFastHLine(4, 12, 3, flagWhite);
        matrix.drawFastHLine(4, 4, 3, flagWhite);
        matrix.drawFastHLine(6, 13, 2, flagWhite);
        matrix.drawFastHLine(6, 3, 2, flagWhite);

        // stars
        matrix.drawRect(8, 7, 2, 2, flagWhite);
        matrix.drawRect(9, 10, 2, 2, flagWhite);
        matrix.drawRect(11, 5, 2, 2, flagWhite);
        matrix.drawRect(13, 10, 2, 2, flagWhite);
        matrix.drawRect(14, 7, 2, 2, flagWhite);

        break;
      }
  }

  matrix.show();

  Serial.println(F("Screen updated"));
}

static void connectToWiFi() {
  Serial.println(F("Connecting to WiFi"));
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    millisDelay(500UL);
  }
  Serial.println(F("WiFi connected"));
  millisDelay(100UL);  // let TLS stack settle
}

void setup() {
  matrix.begin();
  matrix.setBrightness(11);  // UNO R4 WiFi can draw maximum 2A from 5V pin when powered via USB
  matrix.fillScreen(0);
  matrix.show();

  Serial.begin(115200);
  millisDelay(500UL);  // Wait for serial to be ready
}

void loop() {
  static unsigned long lastUpdate = SCREEN_UPDATE_INTERVAL_MS;
  if (millis() - lastUpdate >= SCREEN_UPDATE_INTERVAL_MS) {
    if (WiFi.status() != WL_CONNECTED) {
      connectToWiFi();
    }
    updateScreen();
    lastUpdate = millis();
  }
}
