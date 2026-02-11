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

// ── WiFi Configuration ──────────────────────────────────────────────────────
// Replace with your WiFi credentials
const char* WIFI_SSID = "WIFISSID";
const char* WIFI_PASS = "WIFIPASSWORD";

// ── Timezone Configuration ───────────────────────────────────────────────────
// US Eastern: GMT-5, +1h for daylight saving time
const long GMT_OFFSET_SEC      = -6 * 3600;   // EST = UTC-5
const int  DAYLIGHT_OFFSET_SEC = 3600;         // +1h for EDT

// ── GitHub Status API ───────────────────────────────────────────────────────
const char* API_URL = "https://www.githubstatus.com/api/v2/components.json";

// ── Refresh interval: 2 minutes ─────────────────────────────────────────────
const unsigned long REFRESH_INTERVAL = 120000;

// ── Screensaver: triggers after 5 minutes of no touch ───────────────────────
const unsigned long SCREENSAVER_TIMEOUT = 300000;   // 5 minutes
const unsigned long MATRIX_FRAME_MS     = 50;       // ~20 fps

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

// ── Timing ──────────────────────────────────────────────────────────────────
unsigned long lastRefreshTime = 0;
unsigned long lastTouchTime   = 0;
unsigned long lastFrameTime   = 0;

// ── Screensaver State ───────────────────────────────────────────────────────
bool screensaverActive = false;
bool allOperational    = true;      // Tracks if all components are healthy

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
static const char MATRIX_CHARS[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "@#$%&*+=<>?/|{}[]~^";
static const int MATRIX_CHARS_LEN = sizeof(MATRIX_CHARS) - 1;

static char randomMatrixChar() {
    return MATRIX_CHARS[random(MATRIX_CHARS_LEN)];
}

// ═════════════════════════════════════════════════════════════════════════════
//  Matrix Screensaver Functions (column-strip sprite for smooth rendering)
// ═════════════════════════════════════════════════════════════════════════════
// Instead of a full-screen sprite (~1.8MB), we use a single column-wide
// sprite that is reused for each column.  This keeps memory usage tiny
// while still providing flicker-free per-column rendering.
static LGFX_Sprite* colSprite = nullptr;

// Per-column character buffer so trails persist across frames
#define MATRIX_ROWS_MAX (720 / MATRIX_CHAR_H + 2)
static char   matrixChars[MATRIX_MAX_COLS][MATRIX_ROWS_MAX];
static uint16_t matrixColors[MATRIX_MAX_COLS][MATRIX_ROWS_MAX];

static void matrixInit() {
    int numCols = screenW / MATRIX_CHAR_W;
    for (int i = 0; i < numCols && i < MATRIX_MAX_COLS; i++) {
        matrixCols[i].headY      = -(random(screenH));
        matrixCols[i].speed      = random(7, 16);
        matrixCols[i].length     = random(8, 24);
        matrixCols[i].active     = true;
        matrixCols[i].spawnDelay = 0;
    }
    // Initialize character grid
    memset(matrixChars, 0, sizeof(matrixChars));
    memset(matrixColors, 0, sizeof(matrixColors));
}

static void matrixStartScreensaver() {
    screensaverActive = true;

    // Create a single column-wide sprite (reused for each column)
    if (!colSprite) {
        colSprite = new LGFX_Sprite(&display);
        colSprite->setColorDepth(16);
        colSprite->createSprite(MATRIX_CHAR_W, screenH);
        colSprite->setFont(&fonts::DejaVu24);
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

    for (int i = 0; i < numCols && i < MATRIX_MAX_COLS; i++) {
        MatrixColumn& col = matrixCols[i];

        // Respawn logic for inactive columns
        if (!col.active) {
            if (col.spawnDelay > 0) {
                col.spawnDelay--;
            } else {
                col.headY      = -(random(MATRIX_CHAR_H * 4));
                col.speed      = random(7, 16);
                col.length     = random(8, 24);
                col.active     = true;
            }
        }

        // Advance the column
        if (col.active) {
            col.headY += col.speed;
            if (col.headY - col.length * MATRIX_CHAR_H > screenH) {
                col.active     = false;
                col.spawnDelay = random(5, 40);
            }
        }

        // ── Render this column into the strip sprite ──
        int16_t x = i * MATRIX_CHAR_W;
        colSprite->fillScreen(0x0000);   // Black background

        for (int row = 0; row < numRows; row++) {
            int16_t charY = row * MATRIX_CHAR_H;
            int distFromHead = (col.headY - charY) / MATRIX_CHAR_H;

            if (col.active && distFromHead >= 0 && distFromHead < col.length) {
                // This cell is within the active trail
                if (distFromHead == 0) {
                    // Head — white, new character
                    matrixChars[i][row] = randomMatrixChar();
                    matrixColors[i][row] = 0xFFFF;
                } else if (distFromHead <= 2) {
                    // Near head — bright, occasionally change char
                    if (random(4) == 0) matrixChars[i][row] = randomMatrixChar();
                    uint8_t r, g, b;
                    matrixTrailColor(distFromHead, col.length, r, g, b);
                    matrixColors[i][row] = colSprite->color565(r, g, b);
                } else {
                    // Trail — dimming
                    if (random(8) == 0) matrixChars[i][row] = randomMatrixChar();
                    uint8_t r, g, b;
                    matrixTrailColor(distFromHead, col.length, r, g, b);
                    matrixColors[i][row] = colSprite->color565(r, g, b);
                }
            } else {
                // Fade existing characters toward black
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

            // Draw the character if it has color
            if (matrixColors[i][row] != 0 && matrixChars[i][row] != 0) {
                char ch[2] = { matrixChars[i][row], '\0' };
                colSprite->setTextColor(matrixColors[i][row]);
                colSprite->drawString(ch, 0, charY);
            }
        }

        // Push this column strip to the display
        colSprite->pushSprite(x, 0);
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
//  Fetch status from GitHub API and update the UI
// ═════════════════════════════════════════════════════════════════════════════
static void fetchGitHubStatus() {
    // Reconnect WiFi if needed
    if (WiFi.status() != WL_CONNECTED) {
        statusBar.setRightText("WiFi disconnected");
        if (!screensaverActive) {
            ui.drawAll();
            drawGridLines();
        }
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();                       // Skip certificate verification

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
            bool nowAllOperational = true;
            bool statusChanged = false;
            uint8_t worstSeverity = 0;

            for (JsonObject comp : components) {
                if (idx >= MAX_COMPONENTS) break;

                const char* name   = comp["name"];
                const char* status = comp["status"];
                bool hidden        = comp["only_show_if_degraded"] | false;

                // Skip utility / hidden entries
                if (hidden) continue;
                if (name && strncmp(name, "Visit", 5) == 0) continue;

                // Detect if this component's status changed from last fetch
                if (strcmp(prevStatus[idx], status) != 0) {
                    statusChanged = true;
                    strncpy(prevStatus[idx], status, sizeof(prevStatus[idx]) - 1);
                    prevStatus[idx][sizeof(prevStatus[idx]) - 1] = '\0';
                }

                // Track if any component is NOT operational
                if (strcmp(status, "major_outage") == 0) {
                    nowAllOperational = false;
                    worstSeverity = 2;
                } else if (strcmp(status, "degraded_performance") == 0 ||
                           strcmp(status, "partial_outage") == 0) {
                    nowAllOperational = false;
                    if (worstSeverity < 1) worstSeverity = 1;
                } else if (strcmp(status, "operational") != 0) {
                    nowAllOperational = false;
                }

                idx++;
            }

            // If any status changed, exit screensaver to show the update
            if (statusChanged && screensaverActive) {
                matrixStopScreensaver();
            }
            allOperational = nowAllOperational;
            matrixSeverity = worstSeverity;

            // Only update UI widget labels when not in screensaver
            // (avoids dirty redraws flashing over the matrix animation)
            if (!screensaverActive) {
                int idx2 = 0;
                for (JsonObject comp : components) {
                    if (idx2 >= MAX_COMPONENTS) break;

                    const char* name   = comp["name"];
                    const char* status = comp["status"];
                    bool hidden        = comp["only_show_if_degraded"] | false;
                    if (hidden) continue;
                    if (name && strncmp(name, "Visit", 5) == 0) continue;

                    uint32_t color = statusColor(status);

                    nameLabels[idx2]->setText(name);
                    statusLabels[idx2]->setText(statusDisplayText(status));
                    statusLabels[idx2]->setTextColor(color);
                    statusIcons[idx2]->setFillColor(color);
                    statusIcons[idx2]->setBorderColor(color);
                    statusIcons[idx2]->setIconChar(statusIconChar(status));

                    idx2++;
                }
            }

            // Show last-updated time in the status bar
            struct tm ti;
            if (getLocalTime(&ti, 1000)) {
                char buf[48];
                strftime(buf, sizeof(buf), "Updated: %H:%M:%S", &ti);
                statusBar.setRightText(buf);
            } else {
                statusBar.setRightText("Updated");
            }
        } else {
            statusBar.setRightText("JSON parse error");
            Serial.printf("deserializeJson: %s\n", err.c_str());
        }
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "HTTP error: %d", httpCode);
        statusBar.setRightText(buf);
        Serial.printf("HTTP GET failed: %d\n", httpCode);
    }

    http.end();

    if (!screensaverActive) {
        ui.drawAll();
        drawGridLines();
    }
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

    // ── Grid geometry ──
    contentTop    = TAB5_TITLE_H;
    contentBottom = screenH - TAB5_STATUS_H;
    contentH      = contentBottom - contentTop;
    colW          = screenW / GRID_COLS;
    rowH          = contentH / GRID_ROWS;

    // ── Title bar — left-aligned title ──
    titleBar.setLeftText("GitHub Status");

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

    // ── First status fetch ──
    fetchGitHubStatus();
    lastRefreshTime = millis();
    lastTouchTime   = millis();

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
    // UIManager already consumed the touch, so we check indirectly:
    // If the screen was asleep and UIManager just woke it, that was a touch.
    // For normal operation, poll touch separately just for idle detection.
    // We use a second getTouch call — if UIManager already consumed it,
    // this returns 0, which is fine (no false positive). If there's a
    // continued press, both see it.
    {
        lgfx::touch_point_t tp;
        // Check if display is being touched right now
        auto touchCount = display.getTouch(&tp, 1);
        if (touchCount > 0) {
            lastTouchTime = millis();
            if (screensaverActive) {
                matrixStopScreensaver();
            }
        }
    }

    // ── Refresh GitHub status every 2 minutes (always, even during screensaver) ──
    if (millis() - lastRefreshTime >= REFRESH_INTERVAL) {
        fetchGitHubStatus();
        lastRefreshTime = millis();
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
