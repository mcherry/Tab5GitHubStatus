/*******************************************************************************
 * Tab5GitHubStatus.ino — GitHub Status Dashboard for M5Stack Tab5
 *
 * Displays real-time GitHub service status in a 2×5 grid layout using the
 * Tab5UI library.  Fetches component status from the GitHub Status API
 * every 2 minutes and shows colored circle icons for each service.
 *
 * Required Libraries:
 *   - M5GFX            (Arduino Library Manager)
 *   - Tab5UI           (local)
 *   - ArduinoJson v7+  (Arduino Library Manager)
 *
 * Board: M5Stack Tab5
 ******************************************************************************/

#include <M5GFX.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "Tab5UI.h"
#include "matrix_glyphs.h"

// ── WiFi Configuration ──────────────────────────────────────────────────────
// Replace with your WiFi credentials
const char* WIFI_SSID = "WIFISSID";
const char* WIFI_PASS = "WIFIPASSWORD";

// ── Timezone Configuration ───────────────────────────────────────────────────
// US Eastern: GMT-5, +1h for daylight saving time
const long GMT_OFFSET_SEC      = -6 * 3600;   // CST = UTC-6
const int  DAYLIGHT_OFFSET_SEC = 3600;         // +1h for CDT

// ── GitHub Status API ───────────────────────────────────────────────────────
const char* API_URL       = "https://www.githubstatus.com/api/v2/components.json";
const char* INCIDENTS_URL = "https://www.githubstatus.com/api/v2/incidents/unresolved.json";

// ── Refresh interval: 2 minutes ─────────────────────────────────────────────
const unsigned long REFRESH_INTERVAL = 120000;

// ── Screensaver: triggers after 5 minutes of no touch ───────────────────────
const unsigned long SCREENSAVER_TIMEOUT = 300000;   // 5 minutes
const unsigned long MATRIX_FRAME_MS     = 33;       // ~30 fps

// ── Display & UI ────────────────────────────────────────────────────────────
M5GFX      display;
UIManager  ui(display);

// ── Layout ──────────────────────────────────────────────────────────────────
#define MAX_COMPONENTS  10
#define GRID_COLS        2
#define GRID_ROWS        5

static int16_t screenW, screenH;
static int16_t contentTop, contentBottom, contentH;
static int16_t colW, rowH;

// ── UI Elements ─────────────────────────────────────────────────────────────
UITitleBar   titleBar("");
UIStatusBar  statusBar("");

UILabel*      nameLabels[MAX_COMPONENTS];
UILabel*      statusLabels[MAX_COMPONENTS];
UIIconCircle* statusIcons[MAX_COMPONENTS];

// Status banner between title bar and grid
#define BANNER_H  36
UILabel       statusBanner(0, TAB5_TITLE_H, 1280, BANNER_H,
                           "All Systems Operational",
                           Tab5Theme::TEXT_PRIMARY,
                           TAB5_FONT_SIZE_MD);

// ── Timing ──────────────────────────────────────────────────────────────────
unsigned long lastTouchTime   = 0;
unsigned long lastFrameTime   = 0;

// ── Screensaver State ───────────────────────────────────────────────────────
bool screensaverActive = false;
bool allOperational    = true;      // Tracks if all components are healthy
bool hasUnresolvedIncidents = false; // Tracks unresolved incidents

// Cached status bar text (applied when screensaver exits)
char cachedStatusText[48] = "";

// ═════════════════════════════════════════════════════════════════════════════
//  Background Fetch — Shared data between Core 0 (fetch) and Core 1 (UI)
// ═════════════════════════════════════════════════════════════════════════════
struct ComponentResult {
    char     name[48];
    char     status[32];
};

struct FetchResult {
    bool              valid;             // Data was fetched successfully
    bool              statusChanged;     // At least one component status changed
    bool              allOperational;
    uint8_t           worstSeverity;
    bool              hasIncidents;
    int               componentCount;
    ComponentResult   components[MAX_COMPONENTS];
    char              statusBarText[48]; // Formatted update time
};

static SemaphoreHandle_t fetchMutex = nullptr;
static FetchResult       fetchResult;       // Written by Core 0, read by Core 1
static volatile bool     newDataReady = false;
static TaskHandle_t      fetchTaskHandle = nullptr;

// Matrix color based on overall status (RGB components for trail rendering)
// 0=all operational (green), 1=degraded/partial (orange), 2=major outage (red)
uint8_t matrixSeverity = 0;

// Returns RGB components for the matrix trail based on current severity
static void matrixTrailColor(int distFromHead, int trailLen, uint8_t& r, uint8_t& g, uint8_t& b) {
    float brightness;
    if (distFromHead <= 2) {
        brightness = 1.0f - (distFromHead * 0.15f);
    } else {
        brightness = max(0.08f, (float)(trailLen - distFromHead) / (float)trailLen);
    }

    switch (matrixSeverity) {
        case 2:  // Major outage — red
            r = (uint8_t)(255 * brightness);
            g = (uint8_t)(40 * brightness);
            b = (uint8_t)(30 * brightness);
            break;
        case 1:  // Degraded/partial — orange
            r = (uint8_t)(255 * brightness);
            g = (uint8_t)(150 * brightness);
            b = 0;
            break;
        default: // All operational — green
            r = 0;
            g = (uint8_t)(255 * brightness);
            b = 0;
            break;
    }
}

// ── Previous status tracking (to detect changes) ────────────────────────────
char prevStatus[MAX_COMPONENTS][32];  // Stores last-known status per component

// ── Matrix Rain Columns ─────────────────────────────────────────────────────
#define MATRIX_CHAR_W   22
#define MATRIX_CHAR_H   30
#define MATRIX_MAX_COLS (1280 / MATRIX_CHAR_W + 1)

struct MatrixColumn {
    int16_t  headY;       // Current head position (pixels)
    int16_t  speed;       // Pixels per frame
    int16_t  length;      // Trail length in characters
    bool     active;      // Currently dropping
    uint8_t  spawnDelay;  // Frames until next respawn
};

MatrixColumn matrixCols[MATRIX_MAX_COLS];

// ── Matrix Character Set ────────────────────────────────────────────────────
static uint8_t randomGlyphIndex() {
    return (uint8_t)random(MGLYPH_COUNT);
}

// Draw a 1-bit glyph into a sprite at (dx, dy) with the given RGB565 color.
// The glyph is centered within a MATRIX_CHAR_W x MATRIX_CHAR_H cell.
static void drawGlyph(LGFX_Sprite* spr, int16_t dx, int16_t dy, uint8_t idx, uint16_t color) {
    if (idx >= MGLYPH_COUNT) return;
    const uint8_t* glyph = matrixGlyphs[idx];
    // Center the 16x24 glyph in the 22x30 cell
    int16_t offX = (MATRIX_CHAR_W - MGLYPH_W) / 2;
    int16_t offY = (MATRIX_CHAR_H - MGLYPH_H) / 2;
    for (int gy = 0; gy < MGLYPH_H; gy++) {
        uint8_t b0 = pgm_read_byte(&glyph[gy * MGLYPH_BYTES_PER_ROW]);
        uint8_t b1 = pgm_read_byte(&glyph[gy * MGLYPH_BYTES_PER_ROW + 1]);
        uint16_t rowBits = ((uint16_t)b0 << 8) | b1;
        if (rowBits == 0) continue;  // Skip empty rows
        for (int gx = 0; gx < MGLYPH_W; gx++) {
            if (rowBits & (0x8000 >> gx)) {
                spr->drawPixel(dx + offX + gx, dy + offY + gy, color);
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Matrix Screensaver Functions (column-strip sprite rendering)
// ═════════════════════════════════════════════════════════════════════════════
static LGFX_Sprite* colSprite = nullptr;

// Per-column glyph buffer so trails persist across frames
#define MATRIX_ROWS_MAX (720 / MATRIX_CHAR_H + 2)
static uint8_t  matrixGlyphIdx[MATRIX_MAX_COLS][MATRIX_ROWS_MAX];
static uint16_t matrixColors[MATRIX_MAX_COLS][MATRIX_ROWS_MAX];

static void matrixInit() {
    int numCols = screenW / MATRIX_CHAR_W;
    for (int i = 0; i < numCols && i < MATRIX_MAX_COLS; i++) {
        matrixCols[i].headY      = -(random(screenH));
        matrixCols[i].speed      = random(14, 26);
        matrixCols[i].length     = random(8, 24);
        matrixCols[i].active     = true;
        matrixCols[i].spawnDelay = 0;
    }
    memset(matrixGlyphIdx, 0, sizeof(matrixGlyphIdx));
    memset(matrixColors, 0, sizeof(matrixColors));
}

static void matrixStartScreensaver() {
    screensaverActive = true;

    if (!colSprite) {
        colSprite = new LGFX_Sprite(&display);
        colSprite->setColorDepth(16);
        colSprite->createSprite(MATRIX_CHAR_W, screenH);
    }

    display.fillScreen(0x000000);
    matrixInit();
    lastFrameTime = millis();
}

static void matrixStopScreensaver() {
    screensaverActive = false;

    if (colSprite) {
        colSprite->deleteSprite();
        delete colSprite;
        colSprite = nullptr;
    }

    // Restore deferred UI state that was skipped during screensaver
    updateStatusBanner();
    statusBar.setRightText(cachedStatusText);

    display.setFont(&fonts::DejaVu18);
    ui.clearScreen();
    ui.drawAll();
    drawGridLines();
    lastTouchTime = millis();
}

static void matrixDrawFrame() {
    if (!colSprite) return;

    int numCols = screenW / MATRIX_CHAR_W;
    int numRows = screenH / MATRIX_CHAR_H;

    // Update all column state
    for (int i = 0; i < numCols && i < MATRIX_MAX_COLS; i++) {
        MatrixColumn& col = matrixCols[i];
        if (!col.active) {
            if (col.spawnDelay > 0) {
                col.spawnDelay--;
            } else {
                col.headY      = -(random(MATRIX_CHAR_H * 4));
                col.speed      = random(14, 26);
                col.length     = random(8, 24);
                col.active     = true;
            }
        }
        if (col.active) {
            col.headY += col.speed;
            if (col.headY - col.length * MATRIX_CHAR_H > screenH) {
                col.active     = false;
                col.spawnDelay = random(5, 40);
            }
        }
    }

    // Render each column into the strip sprite and push
    for (int i = 0; i < numCols && i < MATRIX_MAX_COLS; i++) {
        MatrixColumn& col = matrixCols[i];
        int16_t x = i * MATRIX_CHAR_W;

        colSprite->fillScreen(0x0000);

        for (int row = 0; row < numRows; row++) {
            int16_t charY = row * MATRIX_CHAR_H;
            int distFromHead = (col.headY - charY) / MATRIX_CHAR_H;

            if (col.active && distFromHead >= 0 && distFromHead < col.length) {
                if (distFromHead == 0) {
                    matrixGlyphIdx[i][row] = randomGlyphIndex();
                    matrixColors[i][row] = 0xFFFF;
                } else if (distFromHead <= 2) {
                    if (random(4) == 0) matrixGlyphIdx[i][row] = randomGlyphIndex();
                    uint8_t r, g, b;
                    matrixTrailColor(distFromHead, col.length, r, g, b);
                    matrixColors[i][row] = colSprite->color565(r, g, b);
                } else {
                    if (random(8) == 0) matrixGlyphIdx[i][row] = randomGlyphIndex();
                    uint8_t r, g, b;
                    matrixTrailColor(distFromHead, col.length, r, g, b);
                    matrixColors[i][row] = colSprite->color565(r, g, b);
                }
            } else {
                if (matrixColors[i][row] != 0) {
                    uint16_t c = matrixColors[i][row];
                    uint16_t r = (c >> 11) & 0x1F;
                    uint16_t g = (c >> 5)  & 0x3F;
                    uint16_t b =  c        & 0x1F;
                    r = (r > 2) ? r - 2 : 0;
                    g = (g > 4) ? g - 4 : 0;
                    b = (b > 2) ? b - 2 : 0;
                    matrixColors[i][row] = (r << 11) | (g << 5) | b;
                }
            }

            if (matrixColors[i][row] != 0) {
                drawGlyph(colSprite, 0, charY, matrixGlyphIdx[i][row], matrixColors[i][row]);
            }
        }

        colSprite->pushSprite(x, 0);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Update status banner based on current state
// ═════════════════════════════════════════════════════════════════════════════
static void updateStatusBanner() {
    if (allOperational && !hasUnresolvedIncidents) {
        statusBanner.setText("All Systems Operational");
        statusBanner.setBgColor(Tab5Theme::SECONDARY);     // Green
    } else if (matrixSeverity >= 2) {
        statusBanner.setText("Disruption with some GitHub services");
        statusBanner.setBgColor(Tab5Theme::DANGER);         // Red
    } else {
        statusBanner.setText("Disruption with some GitHub services");
        statusBanner.setBgColor(Tab5Theme::ACCENT);         // Orange
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Status → color / display text / icon character
// ═════════════════════════════════════════════════════════════════════════════
static uint32_t statusColor(const char* s) {
    if (strcmp(s, "operational") == 0)          return Tab5Theme::SECONDARY;  // Green
    if (strcmp(s, "degraded_performance") == 0) return Tab5Theme::ACCENT;     // Orange
    if (strcmp(s, "partial_outage") == 0)       return Tab5Theme::ACCENT;
    if (strcmp(s, "major_outage") == 0)         return Tab5Theme::DANGER;     // Red
    return Tab5Theme::TEXT_DISABLED;
}

static const char* statusDisplayText(const char* s) {
    if (strcmp(s, "operational") == 0)          return "Operational";
    if (strcmp(s, "degraded_performance") == 0) return "Degraded Performance";
    if (strcmp(s, "partial_outage") == 0)       return "Partial Outage";
    if (strcmp(s, "major_outage") == 0)         return "Major Outage";
    return "Unknown";
}

static const char* statusIconChar(const char* s) {
    if (strcmp(s, "operational") == 0)          return "";   // Solid green dot
    if (strcmp(s, "degraded_performance") == 0) return "!";
    if (strcmp(s, "partial_outage") == 0)       return "!";
    if (strcmp(s, "major_outage") == 0)         return "X";
    return "?";
}

// ═════════════════════════════════════════════════════════════════════════════
//  Background fetch task (runs on Core 0)
// ═════════════════════════════════════════════════════════════════════════════
static void fetchTaskFunc(void* param) {
    for (;;) {
        // Wait for the refresh interval
        vTaskDelay(pdMS_TO_TICKS(REFRESH_INTERVAL));

        if (WiFi.status() != WL_CONNECTED) {
            // Write a WiFi error result
            if (xSemaphoreTake(fetchMutex, pdMS_TO_TICKS(100))) {
                fetchResult.valid = false;
                strncpy(fetchResult.statusBarText, "WiFi disconnected", sizeof(fetchResult.statusBarText));
                newDataReady = true;
                xSemaphoreGive(fetchMutex);
            }
            continue;
        }

        // ── Local temporaries for building result ──
        FetchResult res;
        memset(&res, 0, sizeof(res));
        res.valid = false;
        res.allOperational = true;
        res.worstSeverity = 0;
        res.hasIncidents = false;
        res.statusChanged = false;

        // ── Fetch components ──
        {
            WiFiClientSecure client;
            client.setInsecure();
            HTTPClient http;
            http.begin(client, API_URL);
            http.setTimeout(10000);
            int httpCode = http.GET();

            if (httpCode == HTTP_CODE_OK) {
                String payload = http.getString();
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, payload);

                if (!err) {
                    JsonArray components = doc["components"];
                    int idx = 0;

                    for (JsonObject comp : components) {
                        if (idx >= MAX_COMPONENTS) break;

                        const char* name   = comp["name"];
                        const char* status = comp["status"];
                        bool hidden        = comp["only_show_if_degraded"] | false;

                        if (hidden) continue;
                        if (name && strncmp(name, "Visit", 5) == 0) continue;

                        strncpy(res.components[idx].name, name, sizeof(res.components[idx].name) - 1);
                        strncpy(res.components[idx].status, status, sizeof(res.components[idx].status) - 1);

                        // Detect changes against prevStatus (read-only, safe)
                        if (strcmp(prevStatus[idx], status) != 0) {
                            res.statusChanged = true;
                        }

                        if (strcmp(status, "major_outage") == 0) {
                            res.allOperational = false;
                            res.worstSeverity = 2;
                        } else if (strcmp(status, "degraded_performance") == 0 ||
                                   strcmp(status, "partial_outage") == 0) {
                            res.allOperational = false;
                            if (res.worstSeverity < 1) res.worstSeverity = 1;
                        } else if (strcmp(status, "operational") != 0) {
                            res.allOperational = false;
                        }

                        idx++;
                    }
                    res.componentCount = idx;
                    res.valid = true;

                    // Format update time
                    struct tm ti;
                    if (getLocalTime(&ti, 1000)) {
                        strftime(res.statusBarText, sizeof(res.statusBarText), "Updated: %H:%M:%S", &ti);
                    } else {
                        strncpy(res.statusBarText, "Updated", sizeof(res.statusBarText));
                    }
                } else {
                    strncpy(res.statusBarText, "JSON parse error", sizeof(res.statusBarText));
                    Serial.printf("deserializeJson: %s\n", err.c_str());
                }
            } else {
                snprintf(res.statusBarText, sizeof(res.statusBarText), "HTTP error: %d", httpCode);
                Serial.printf("HTTP GET failed: %d\n", httpCode);
            }
            http.end();
        }

        // ── Fetch unresolved incidents ──
        {
            WiFiClientSecure client;
            client.setInsecure();
            HTTPClient http;
            http.begin(client, INCIDENTS_URL);
            http.setTimeout(10000);
            int httpCode = http.GET();

            if (httpCode == HTTP_CODE_OK) {
                String payload = http.getString();
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, payload);
                if (!err) {
                    JsonArray incidents = doc["incidents"];
                    res.hasIncidents = (incidents.size() > 0);
                }
            }
            http.end();
        }

        // Factor incidents into severity
        if (res.hasIncidents && res.worstSeverity < 1) {
            res.worstSeverity = 1;
        }

        // ── Write result to shared struct under mutex ──
        if (xSemaphoreTake(fetchMutex, pdMS_TO_TICKS(200))) {
            memcpy(&fetchResult, &res, sizeof(FetchResult));
            newDataReady = true;
            xSemaphoreGive(fetchMutex);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Apply fetch results to UI (called on Core 1 in loop)
// ═════════════════════════════════════════════════════════════════════════════
static void applyFetchResults() {
    FetchResult res;

    // Copy result under mutex
    if (!xSemaphoreTake(fetchMutex, pdMS_TO_TICKS(50))) return;
    memcpy(&res, &fetchResult, sizeof(FetchResult));
    newDataReady = false;
    xSemaphoreGive(fetchMutex);

    // Update cached status text
    strncpy(cachedStatusText, res.statusBarText, sizeof(cachedStatusText));

    if (!res.valid) {
        // WiFi disconnected or parse error — just update status bar
        if (!screensaverActive) {
            statusBar.setRightText(cachedStatusText);
            ui.drawAll();
            drawGridLines();
        }
        return;
    }

    // Update prevStatus and detect changes
    bool statusChanged = false;
    for (int i = 0; i < res.componentCount; i++) {
        if (strcmp(prevStatus[i], res.components[i].status) != 0) {
            statusChanged = true;
            strncpy(prevStatus[i], res.components[i].status, sizeof(prevStatus[i]) - 1);
            prevStatus[i][sizeof(prevStatus[i]) - 1] = '\0';
        }
    }

    // Check if incident state changed
    bool incidentChanged = (hasUnresolvedIncidents != res.hasIncidents);

    // Update global state
    allOperational = res.allOperational;
    hasUnresolvedIncidents = res.hasIncidents;
    matrixSeverity = res.worstSeverity;

    // If status or incidents changed, exit screensaver
    if ((statusChanged || incidentChanged) && screensaverActive) {
        matrixStopScreensaver();
    }

    // Update UI widgets only when screensaver is not active
    if (!screensaverActive) {
        for (int i = 0; i < res.componentCount; i++) {
            uint32_t color = statusColor(res.components[i].status);

            nameLabels[i]->setText(res.components[i].name);
            statusLabels[i]->setText(statusDisplayText(res.components[i].status));
            statusLabels[i]->setTextColor(color);
            statusIcons[i]->setFillColor(color);
            statusIcons[i]->setBorderColor(color);
            statusIcons[i]->setIconChar(statusIconChar(res.components[i].status));
        }

        updateStatusBanner();
        statusBar.setRightText(cachedStatusText);
        ui.drawAll();
        drawGridLines();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Draw subtle grid dividers between cells
// ═════════════════════════════════════════════════════════════════════════════
static void drawGridLines() {
    for (int row = 1; row < GRID_ROWS; row++) {
        int16_t y = contentTop + row * rowH;
        display.drawFastHLine(0, y, screenW, Tab5Theme::DIVIDER);
    }
    display.drawFastVLine(colW, contentTop, contentH, Tab5Theme::DIVIDER);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Initial synchronous fetch (used once in setup before task starts)
// ═════════════════════════════════════════════════════════════════════════════
static void fetchGitHubStatusSync() {
    if (WiFi.status() != WL_CONNECTED) {
        statusBar.setRightText("WiFi disconnected");
        ui.drawAll();
        drawGridLines();
        return;
    }

    // Fetch components
    {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, API_URL);
        http.setTimeout(10000);
        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);

            if (!err) {
                JsonArray components = doc["components"];
                int idx = 0;
                uint8_t worstSeverity = 0;

                for (JsonObject comp : components) {
                    if (idx >= MAX_COMPONENTS) break;
                    const char* name   = comp["name"];
                    const char* status = comp["status"];
                    bool hidden        = comp["only_show_if_degraded"] | false;
                    if (hidden) continue;
                    if (name && strncmp(name, "Visit", 5) == 0) continue;

                    strncpy(prevStatus[idx], status, sizeof(prevStatus[idx]) - 1);
                    uint32_t color = statusColor(status);
                    nameLabels[idx]->setText(name);
                    statusLabels[idx]->setText(statusDisplayText(status));
                    statusLabels[idx]->setTextColor(color);
                    statusIcons[idx]->setFillColor(color);
                    statusIcons[idx]->setBorderColor(color);
                    statusIcons[idx]->setIconChar(statusIconChar(status));

                    if (strcmp(status, "major_outage") == 0) {
                        allOperational = false; worstSeverity = 2;
                    } else if (strcmp(status, "degraded_performance") == 0 ||
                               strcmp(status, "partial_outage") == 0) {
                        allOperational = false;
                        if (worstSeverity < 1) worstSeverity = 1;
                    } else if (strcmp(status, "operational") != 0) {
                        allOperational = false;
                    }
                    idx++;
                }
                matrixSeverity = worstSeverity;

                struct tm ti;
                if (getLocalTime(&ti, 1000)) {
                    strftime(cachedStatusText, sizeof(cachedStatusText), "Updated: %H:%M:%S", &ti);
                } else {
                    strncpy(cachedStatusText, "Updated", sizeof(cachedStatusText));
                }
                statusBar.setRightText(cachedStatusText);
            }
        }
        http.end();
    }

    // Fetch incidents
    {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, INCIDENTS_URL);
        http.setTimeout(10000);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);
            if (!err) {
                JsonArray incidents = doc["incidents"];
                hasUnresolvedIncidents = (incidents.size() > 0);
            }
        }
        http.end();
    }

    if (hasUnresolvedIncidents && matrixSeverity < 1) matrixSeverity = 1;
    updateStatusBanner();
    ui.drawAll();
    drawGridLines();
}

// ═════════════════════════════════════════════════════════════════════════════
//  setup()
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    // ── Display init (landscape 1280×720) ──
    display.init();
    display.setRotation(1);
    Tab5UI::init(display);
    ui.setBrightness(128);
    display.setFont(&fonts::DejaVu18);

    screenW = Tab5UI::screenW();
    screenH = Tab5UI::screenH();

    // ── Grid geometry (account for banner below title bar) ──
    contentTop    = TAB5_TITLE_H + BANNER_H;
    contentBottom = screenH - TAB5_STATUS_H;
    contentH      = contentBottom - contentTop;
    colW          = screenW / GRID_COLS;
    rowH          = contentH / GRID_ROWS;

    // ── Title bar — left-aligned title ──
    titleBar.setLeftText("GitHub Status");

    // ── Status banner — centered, full width ──
    statusBanner.setPosition(0, TAB5_TITLE_H);
    statusBanner.setSize(screenW, BANNER_H);
    statusBanner.setAlign(textdatum_t::middle_center);
    statusBanner.setBgColor(Tab5Theme::SECONDARY);
    statusBanner.setTextColor(Tab5Theme::TEXT_PRIMARY);

    // ── Status bar — right-aligned update time ──
    statusBar.setRightText("Starting...");

    // ── Create per-component UI elements in a 2×5 grid ──
    const int16_t pad   = 24;
    const int16_t iconR = 18;

    for (int i = 0; i < MAX_COMPONENTS; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        int16_t cellX = col * colW;
        int16_t cellY = contentTop + row * rowH;

        // ── Component name ──
        nameLabels[i] = new UILabel(
            cellX + pad,
            cellY + rowH / 2 - 28,
            colW - pad * 2 - iconR * 3,
            32,
            "Loading...",
            Tab5Theme::TEXT_PRIMARY,
            TAB5_FONT_SIZE_MD
        );
        nameLabels[i]->setBgColor(Tab5Theme::BG_DARK);
        nameLabels[i]->setAlign(textdatum_t::middle_left);

        // ── Status text ──
        statusLabels[i] = new UILabel(
            cellX + pad,
            cellY + rowH / 2 + 8,
            colW - pad * 2 - iconR * 3,
            28,
            "",
            Tab5Theme::TEXT_SECONDARY,
            TAB5_FONT_SIZE_SM
        );
        statusLabels[i]->setBgColor(Tab5Theme::BG_DARK);
        statusLabels[i]->setAlign(textdatum_t::middle_left);

        // ── Status icon circle ──
        statusIcons[i] = new UIIconCircle(
            cellX + colW - pad - iconR * 2,
            cellY + (rowH - iconR * 2) / 2,
            iconR,
            Tab5Theme::TEXT_DISABLED,       // Fill: grey until data loads
            Tab5Theme::TEXT_DISABLED        // Border matches fill
        );
    }

    // ── Register all elements ──
    ui.setBackground(Tab5Theme::BG_DARK);
    ui.clearScreen();

    ui.addElement(&titleBar);
    ui.addElement(&statusBanner);
    for (int i = 0; i < MAX_COMPONENTS; i++) {
        ui.addElement(nameLabels[i]);
        ui.addElement(statusLabels[i]);
        ui.addElement(statusIcons[i]);
    }
    ui.addElement(&statusBar);

    ui.setContentArea(contentTop, contentBottom);
    ui.drawAll();
    drawGridLines();

    // ── Connect to WiFi ──
    statusBar.setRightText("Connecting to WiFi...");
    ui.drawAll();
    drawGridLines();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        statusBar.setRightText("WiFi failed!");
        ui.drawAll();
        drawGridLines();
        Serial.println("WiFi connection failed");
        return;
    }

    Serial.printf("Connected: %s\n", WiFi.localIP().toString().c_str());

    // ── NTP time sync ──
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");

    // ── Initialize previous status tracking ──
    memset(prevStatus, 0, sizeof(prevStatus));

    // ── First status fetch (synchronous, before background task starts) ──
    fetchGitHubStatusSync();
    lastTouchTime   = millis();

    // ── Create mutex and start background fetch task on Core 0 ──
    fetchMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(
        fetchTaskFunc,      // Task function
        "FetchTask",        // Name
        8192,               // Stack size (bytes)
        nullptr,            // Parameter
        1,                  // Priority
        &fetchTaskHandle,   // Task handle
        0                   // Core 0
    );
    Serial.println("Background fetch task started on Core 0");

    // ── Screen sleep after 8 hours of no touch ──
    ui.setSleepTimeout(480);
    ui.setLightSleep(false);
}

// ═════════════════════════════════════════════════════════════════════════════
//  loop()
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
    // ── Always let UIManager process touch (handles sleep timeout too) ──
    ui.update();

    // ── Detect touch for screensaver idle tracking ──
    {
        lgfx::touch_point_t tp;
        auto touchCount = display.getTouch(&tp, 1);
        if (touchCount > 0) {
            lastTouchTime = millis();
            if (screensaverActive) {
                matrixStopScreensaver();
            }
        }
    }

    // ── Check for new data from background fetch task (Core 0) ──
    if (newDataReady) {
        applyFetchResults();
    }

    // ── Screensaver activation after 5 min idle ──
    if (!screensaverActive && !ui.isScreenAsleep()
        && (millis() - lastTouchTime >= SCREENSAVER_TIMEOUT)) {
        matrixStartScreensaver();
    }

    // ── Animate matrix rain ──
    if (screensaverActive && !ui.isScreenAsleep()) {
        if (millis() - lastFrameTime >= MATRIX_FRAME_MS) {
            matrixDrawFrame();
            lastFrameTime = millis();
        }
    }

    yield();
}
