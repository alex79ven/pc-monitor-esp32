#include <Arduino.h>
#include <HWCDC.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <math.h>

#ifndef OLED_SDA
#define OLED_SDA 20
#endif
#ifndef OLED_SCL
#define OLED_SCL 21
#endif

#ifndef BLE_NAME
#define BLE_NAME "OLED-MONITOR"
#endif

// 0.96" SSD1306 128x64 two-color OLED:
// upper 16 pixels are yellow, lower 48 pixels are blue.
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_STATUS_H 16
#define OLED_MAIN_Y 18

#define BLE_SERVICE "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_CHAR_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_CHAR_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

#define BLEQ_ITEMS 8
#define BLEQ_ITEM_SIZE 160

#define OSD_DURATION 5000
#define DATA_TIMEOUT_MS 10000
// Если хост уснул, ESP32 может не получить callback отключения. Считаем
// соединение зависшим, если от host давно не было ни одной команды.
#define BLE_STALE_MS 15000
#define BLE_ADV_RETRY_MS 5000

static QueueHandle_t bleQueue;

enum OsdType {
    OSD_NONE = 0,
    OSD_BRIGHT,
    OSD_KBD,
    OSD_VOL,
    OSD_MEDIA
};

enum ModeType {
    MODE_METRICS = 0,
    MODE_CLOCK,
    MODE_STARS,
    MODE_MEDIA
};

static volatile ModeType gMode = MODE_METRICS;
static char gTime[8] = "00:00";
static char gDate[12] = "--.--";
static bool gClockSet = false;
static int gClockSec = 0;
static uint32_t gClockSetMs = 0;
static volatile uint32_t gLastData = 0;
static volatile uint32_t gBleLastData = 0;

static volatile OsdType gOsdType = OSD_NONE;
static volatile int gOsdVal = 0;
static volatile uint32_t gOsdUntil = 0;
static volatile bool gMuted = false;

static int gCpu = -1;
static int gRam = -1;
static int gDisk = -1;
static int gTemp = -1;
static int gKbd = -1;

#define CONTRAST_DEFAULT 180
#define CONTRAST_CLOCK 3

#define STARS_N 28
#define STARS_FRAME_MS 120

static float starX[STARS_N];
static float starY[STARS_N];
static float starZ[STARS_N];
static bool starsInit = false;
static uint32_t lastStarsMs = 0;

#define STARS_BURST 12
static float burstX[STARS_BURST];
static float burstY[STARS_BURST];
static float burstZ[STARS_BURST];
static uint8_t burstLife[STARS_BURST];
static uint8_t burstIdx = 0;

#define MEDIA_TEXT_MAX 120
#define MEDIA_SCROLL_SPEED_MS 55
static char gMediaText[MEDIA_TEXT_MAX] = "";
static int gMediaIcon = 0;      // 0 spotify, 1 youtube
static int gMediaScroll = 0;
static uint32_t gMediaLastMs = 0;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// --- Защита от выгорания OLED ---
// Сдвиг применяется к САМИМ координатам отрисовки, а не к готовому
// буферу кадра. Раньше буфер сдвигался после рисования, но следующая
// перерисовка (clearBuffer + рисование по абсолютным координатам)
// полностью стирала сдвиг, и эффект был нулевым.
// Изображение смещается на 1 px по горизонтали каждые SHIFT_INTERVAL_MS.
// Только по X: вертикальный сдвиг переносил содержимое жёлтой полосы
// (16 px) в синюю область, и граница цветов выглядела бы неаккуратно.
#define SHIFT_STEP_PX 1
#define SHIFT_INTERVAL_MS 60000UL
#define SHIFT_POS_MAX 3

static int gOffX = 0;
static uint32_t gShiftAtMs = 0;

// Обёртки отрисовки: добавляют горизонтальное смещение к координатам.
inline void dPixel(int x, int y) { u8g2.drawPixel(x + gOffX, y); }
inline void dHLine(int x, int y, int w) { u8g2.drawHLine(x + gOffX, y, w); }
inline void dBox(int x, int y, int w, int h) { u8g2.drawBox(x + gOffX, y, w, h); }
inline void dFrame(int x, int y, int w, int h) { u8g2.drawFrame(x + gOffX, y, w, h); }
inline void dRFrame(int x, int y, int w, int h, int r) { u8g2.drawRFrame(x + gOffX, y, w, h, r); }
inline void dLine(int x0, int y0, int x1, int y1) { u8g2.drawLine(x0 + gOffX, y0, x1 + gOffX, y1); }
inline void dCircle(int x, int y, int r) { u8g2.drawCircle(x + gOffX, y, r); }
inline void dDisc(int x, int y, int r) { u8g2.drawDisc(x + gOffX, y, r); }
inline void dTriangle(int x0, int y0, int x1, int y1, int x2, int y2)
{
    u8g2.drawTriangle(x0 + gOffX, y0, x1 + gOffX, y1, x2 + gOffX, y2);
}
inline void dStr(int x, int y, const char* s) { u8g2.drawStr(x + gOffX, y, s); }
inline void dUTF8X2(int x, int y, const char* s) { u8g2.drawUTF8X2(x + gOffX, y, s); }

void drawMetrics();
void drawStars();
void drawMediaScreen();
void drawClock();

void redrawCurrent()
{
    switch (gMode) {
        case MODE_CLOCK: drawClock(); break;
        case MODE_STARS: drawStars(); break;
        case MODE_MEDIA: drawMediaScreen(); break;
        default: drawMetrics(); break;
    }
}

void tickPixelShift()
{
    uint32_t now = millis();
    if (gShiftAtMs == 0) {
        gShiftAtMs = now + SHIFT_INTERVAL_MS;
        return;
    }
    if ((now - gShiftAtMs) < SHIFT_INTERVAL_MS)
        return;
    gShiftAtMs = now;

    gOffX = (gOffX + SHIFT_STEP_PX) % (SHIFT_POS_MAX + 1);
    redrawCurrent();
}

void drawBar(int x, int baseline, int w, int barH, int v)
{
    dFrame(x, baseline - barH + 1, w, barH);
    int fw = (int)((long)(w - 2) * v / 100);
    if (fw > 0)
        dBox(x + 1, baseline - barH + 2, fw, barH - 2);
}

void drawRow(int baseline, const char* label, int value)
{
    u8g2.setFont(u8g2_font_6x10_tf);
    dStr(2, baseline, label);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", value);
    dStr(24, baseline, buf);
    drawBar(54, baseline, 72, 10, value);
}

void applyContrast()
{
    if (gMode == MODE_CLOCK) {
        u8g2.setContrast(CONTRAST_CLOCK);
        return;
    }
    if (gKbd < 0) {
        u8g2.setContrast(CONTRAST_DEFAULT);
        return;
    }
    int c = (int)((long)gKbd * 255 / 100);
    if (c < CONTRAST_CLOCK)
        c = CONTRAST_CLOCK;
    u8g2.setContrast(c);
}

void updateClockTime()
{
    long secElapsed = (long)((millis() - gClockSetMs) / 1000);
    int t = (int)((gClockSec + secElapsed) % 86400);
    snprintf(gTime, sizeof(gTime), "%02d:%02d", t / 3600, (t % 3600) / 60);
}

// В шрифтах logisoso нет глифа градуса, поэтому значок °C рисуется пикселями.
int tempTextWidth(const char* num, bool big)
{
    return u8g2.getStrWidth(num) + (big ? 8 : 5) + u8g2.getStrWidth("C");
}

void drawTempText(int x, int baseline, const char* num, bool big)
{
    dStr(x, baseline, num);
    int dx = x + u8g2.getStrWidth(num) + (big ? 2 : 1);
    if (big) {
        // кольцо 5x5 в верхней части строки знака
        dCircle(dx + 2, baseline - 12, 2);
        dStr(dx + 6, baseline, "C");
    } else {
        dBox(dx, baseline - 8, 3, 3);
        dStr(dx + 4, baseline, "C");
    }
}

void drawStatusBar(const char* left, const char* center, const char* temp)
{
    u8g2.setFont(u8g2_font_5x8_tf);
    if (left && left[0]) {
        dStr(2, 11, left);
    }
    if (center && center[0]) {
        int w = u8g2.getStrWidth(center);
        dStr((OLED_WIDTH - w) / 2, 11, center);
    }
    if (temp && temp[0]) {
        drawTempText(OLED_WIDTH - tempTextWidth(temp, false) - 2, 11, temp, false);
    }
    dHLine(0, OLED_STATUS_H - 1, OLED_WIDTH - 1);
}

void drawMetricsStatusBar()
{
    char temp[8];
    if (gTemp > 0)
        snprintf(temp, sizeof(temp), "%d", gTemp);
    else
        snprintf(temp, sizeof(temp), "--");

    if (gClockSet)
        updateClockTime();
    else
        snprintf(gTime, sizeof(gTime), "--:--");

    // Именно _tf, а не _tn: вариант _tn содержит только цифры и знаки,
    // в нём нет букв (ни "C", ни других).
    u8g2.setFont(u8g2_font_logisoso16_tf);
    drawTempText(2, 15, temp, true);
    int tw = u8g2.getStrWidth(gTime);
    dStr(OLED_WIDTH - tw - 2, 15, gTime);
}

void drawProgressStatusBar(const char* label, int val)
{
    if (val < 0)
        val = 0;
    if (val > 100)
        val = 100;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", val);
    u8g2.setFont(u8g2_font_5x8_tf);
    dStr(2, 7, label);
    int tw = u8g2.getStrWidth(buf);
    dStr(OLED_WIDTH - tw - 2, 7, buf);

    dFrame(2, 8, OLED_WIDTH - 4, 7);
    int fw = (int)((long)(OLED_WIDTH - 8) * val / 100);
    if (fw > 0)
        dBox(3, 9, fw, 5);
}

void drawClock()
{
    updateClockTime();
    u8g2.clearBuffer();

    // Дата крупно в жёлтой полосе: "MON, 5 OCT".
    u8g2.setFont(u8g2_font_logisoso16_tf);
    int dw = u8g2.getStrWidth(gDate);
    dStr((OLED_WIDTH - dw) / 2, 15, gDate);

    // 42 px — максимальный размер, при котором "23:45" помещается
    // в ширину 128 px и по высоте в синюю область (46 px).
    u8g2.setFont(u8g2_font_logisoso42_tn);
    int w = u8g2.getStrWidth(gTime);
    dStr((OLED_WIDTH - w) / 2, 60, gTime);
    u8g2.sendBuffer();
}

void drawMetrics()
{
    u8g2.clearBuffer();

    char temp[8];
    if (gTemp > 0)
        snprintf(temp, sizeof(temp), "%d", gTemp);
    else
        snprintf(temp, sizeof(temp), "--");

    if (gCpu < 0 && gRam < 0 && gDisk < 0) {
        drawStatusBar("PC", "OFFLINE", temp);
        u8g2.setFont(u8g2_font_6x10_tf);
        const char* title = "PC MONITOR";
        const char* waiting = "WAITING FOR HOST";
        int tw = u8g2.getStrWidth(title);
        int ww = u8g2.getStrWidth(waiting);
        dStr((OLED_WIDTH - tw) / 2, 34, title);
        dStr((OLED_WIDTH - ww) / 2, 50, waiting);
        u8g2.sendBuffer();
        return;
    }

    if (gClockSet)
        updateClockTime();
    drawMetricsStatusBar();

    if (gCpu >= 0)
        drawRow(30, "CPU", gCpu);
    if (gRam >= 0)
        drawRow(43, "RAM", gRam);
    if (gDisk >= 0)
        drawRow(56, "DSK", gDisk);

    u8g2.sendBuffer();
}

static float randF(float min, float max)
{
    return min + (float)random() / (float)RAND_MAX * (max - min);
}

void starBurst()
{
    for (int i = 0; i < 5; i++) {
        burstX[burstIdx] = randF(-1.0f, 1.0f);
        burstY[burstIdx] = randF(-1.0f, 1.0f);
        burstZ[burstIdx] = randF(0.05f, 0.5f);
        burstLife[burstIdx] = 3;
        burstIdx = (burstIdx + 1) % STARS_BURST;
    }
}

// Время по центру жёлтой полосы на заставке. logisoso16_tn тонкий,
// его ascent ровно 16 px, поэтому цифры занимают всю высоту полосы.
// Вариант со сжатием крупного шрифта (46 -> 16 px) давал артефакты:
// при выборке одного пикселя из трёх тонкие штрихи разрывались.
void drawStarsClock()
{
    if (!gClockSet)
        return;
    updateClockTime();

    u8g2.setFont(u8g2_font_logisoso16_tn);
    int w = u8g2.getStrWidth(gTime);
    dStr((OLED_WIDTH - w) / 2, OLED_STATUS_H, gTime);
}

void drawStars()
{
    if (!starsInit) {
        randomSeed(esp_random());
        for (int i = 0; i < STARS_N; i++) {
            starX[i] = randF(-1.0f, 1.0f);
            starY[i] = randF(-1.0f, 1.0f);
            starZ[i] = randF(0.0f, 1.0f);
        }
        starsInit = true;
    }

    u8g2.clearBuffer();
    drawStarsClock();

    int cx = OLED_WIDTH / 2;
    int cy = (OLED_MAIN_Y + OLED_HEIGHT) / 2;
    float scaleX = (float)(OLED_WIDTH / 2 - 4);
    float scaleY = (float)((OLED_HEIGHT - OLED_MAIN_Y) / 2 - 2);
    for (int i = 0; i < STARS_N; i++) {
        starZ[i] -= 0.03f;
        if (starZ[i] <= 0.0f) {
            starX[i] = randF(-1.0f, 1.0f);
            starY[i] = randF(-1.0f, 1.0f);
            starZ[i] = 1.0f;
        }
        int px = cx + (int)(starX[i] / starZ[i] * scaleX);
        int py = cy + (int)(starY[i] / starZ[i] * scaleY);
        if (px < 0 || px >= OLED_WIDTH || py < OLED_MAIN_Y || py >= OLED_HEIGHT)
            continue;
        if (starZ[i] < 0.3f)
            dBox(px - 1, py - 1, 3, 3);
        else
            dPixel(px, py);
    }
    for (int i = 0; i < STARS_BURST; i++) {
        if (burstLife[i] == 0)
            continue;
        int px = cx + (int)(burstX[i] / burstZ[i] * scaleX);
        int py = cy + (int)(burstY[i] / burstZ[i] * scaleY);
        if (px >= 0 && px < OLED_WIDTH && py >= OLED_MAIN_Y && py < OLED_HEIGHT)
            dBox(px - 2, py - 2, 5, 5);
        burstLife[i]--;
    }
    u8g2.sendBuffer();
}

void tickStars()
{
    if (gMode != MODE_STARS)
        return;
    uint32_t now = millis();
    if (now - lastStarsMs >= STARS_FRAME_MS) {
        lastStarsMs = now;
        drawStars();
    }
}

void drawBrightIcon(int x, int y)
{
    int cx = x + 6, cy = y + 6;
    dDisc(cx, cy, 3);
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4.0f;
        int x0 = cx + (int)(cosf(a) * 5), y0 = cy + (int)(sinf(a) * 5);
        int x1 = cx + (int)(cosf(a) * 9), y1 = cy + (int)(sinf(a) * 9);
        dLine(x0, y0, x1, y1);
    }
}

void drawVolumeIcon(int x, int y, bool muted)
{
    dBox(x, y + 5, 4, 6);
    dTriangle(x + 4, y + 6, x + 10, y + 2, x + 10, y + 14);
    dCircle(x + 12, y + 8, 2);
    dCircle(x + 12, y + 8, 4);
    if (muted) {
        dLine(x + 10, y + 6, x + 16, y + 12);
        dLine(x + 16, y + 6, x + 10, y + 12);
    }
}

void drawKbdIcon(int x, int y)
{
    dFrame(x, y + 2, 15, 12);
    dBox(x + 2, y + 4, 2, 2);
    dBox(x + 5, y + 4, 2, 2);
    dBox(x + 8, y + 4, 2, 2);
    dBox(x + 11, y + 4, 2, 2);
    dBox(x + 2, y + 8, 4, 2);
    dBox(x + 7, y + 8, 5, 2);
}

void drawMediaIcon(int x, int y, int mode)
{
    switch (mode) {
        case 0: // pause
            dBox(x + 1, y + 1, 4, 13);
            dBox(x + 9, y + 1, 4, 13);
            break;
        case 1: // play
            dTriangle(x + 2, y + 1, x + 2, y + 14, x + 14, y + 8);
            break;
        case 2: // toggle
            dTriangle(x + 1, y + 3, x + 1, y + 12, x + 9, y + 8);
            dBox(x + 10, y + 3, 4, 10);
            break;
        case 3: // next
            dTriangle(x + 1, y + 3, x + 1, y + 12, x + 9, y + 8);
            dBox(x + 10, y + 3, 3, 10);
            break;
        case 4: // prev
            dBox(x + 1, y + 3, 3, 10);
            dTriangle(x + 5, y + 3, x + 5, y + 12, x + 13, y + 8);
            break;
        default:
            break;
    }
}

void drawOsd(OsdType type, int val)
{
    const char* label = "";
    switch (type) {
        case OSD_BRIGHT: label = "BRIGHTNESS"; break;
        case OSD_KBD: label = "KEYBOARD"; break;
        case OSD_VOL: label = gMuted ? "MUTE" : "VOLUME"; break;
        case OSD_MEDIA:
            switch (val) {
                case 0: label = "PAUSE"; break;
                case 1: label = "PLAY"; break;
                case 2: label = "PLAY/PAUSE"; break;
                case 3: label = "NEXT"; break;
                case 4: label = "PREV"; break;
                default: label = "MEDIA"; break;
            }
            break;
        default: return;
    }

    u8g2.clearBuffer();

    if (type == OSD_MEDIA) {
        drawStatusBar("", label, "");
        drawMediaIcon((OLED_WIDTH - 16) / 2, 30, val);
        u8g2.sendBuffer();
        return;
    }

    drawProgressStatusBar(label, val);

    switch (type) {
        case OSD_BRIGHT: drawBrightIcon(12, 24); break;
        case OSD_KBD: drawKbdIcon(11, 24); break;
        case OSD_VOL: drawVolumeIcon(10, 24, gMuted); break;
        default: break;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", val);
    u8g2.setFont(u8g2_font_logisoso24_tn);
    int nw = u8g2.getStrWidth(buf);
    dStr(48, 50, buf);

    u8g2.setFont(u8g2_font_5x8_tf);
    dStr(52 + nw, 38, "%");
    dHLine(12, 58, 115);

    u8g2.sendBuffer();
}

void setOsd(OsdType type, int val)
{
    if (val < 0)
        val = 0;
    if (val > 100)
        val = 100;
    gOsdType = type;
    gOsdVal = val;
    gOsdUntil = millis();
    drawOsd(type, val);
}

void drawSpotifyIcon(int x, int y)
{
    dCircle(x + 8, y + 8, 8);
    dDisc(x + 8, y + 8, 3);
}

void drawYoutubeIcon(int x, int y)
{
    dRFrame(x, y, 19, 13, 2);
    dTriangle(x + 5, y + 3, x + 5, y + 10, x + 13, y + 7);
}

void drawMediaScreen()
{
    u8g2.clearBuffer();

    if (gMediaIcon == 1) {
        drawYoutubeIcon(2, 1);
        u8g2.setFont(u8g2_font_logisoso16_tf);
        const char* label = "YOUTUBE";
        int lw = u8g2.getStrWidth(label);
        dStr(24 + (OLED_WIDTH - 24 - lw) / 2, 15, label);
    } else {
        drawSpotifyIcon(2, 0);
        u8g2.setFont(u8g2_font_5x8_tf);
        dStr(24, 11, "SPOTIFY");
        if (gClockSet) {
            updateClockTime();
            int tw = u8g2.getStrWidth(gTime);
            dStr(OLED_WIDTH - tw - 2, 11, gTime);
        }
    }

    // 8x13 с X2 даёт 14x18 px (было 6x12 с X2 = 10x14).
    u8g2.setFont(u8g2_font_8x13_t_cyrillic);
    if (!gMediaText[0]) {
        const char* empty = "NO MEDIA";
        int w = u8g2.getUTF8Width(empty) * 2;
        dUTF8X2((OLED_WIDTH - w) / 2, 52, empty);
    } else {
        int textW = u8g2.getUTF8Width(gMediaText) * 2;
        if (textW <= OLED_WIDTH) {
            dUTF8X2((OLED_WIDTH - textW) / 2, 52, gMediaText);
        } else {
            int full = textW + OLED_WIDTH;
            int sx = OLED_WIDTH - (gMediaScroll % full);
            dUTF8X2(sx, 52, gMediaText);
        }
    }
    u8g2.sendBuffer();
}

void setMedia(const char* icon, const char* text)
{
    int ic = atoi(icon);
    ic = (ic == 1) ? 1 : 0;
    bool wasMedia = (gMode == MODE_MEDIA);
    bool same = wasMedia && ic == gMediaIcon &&
                strcmp(gMediaText, text) == 0;
    if (same)
        return;
    bool entering = !wasMedia;
    gMediaIcon = ic;
    snprintf(gMediaText, sizeof(gMediaText), "%s", text);
    if (entering)
        gMediaScroll = 0;
    gMediaLastMs = millis();
    gMode = MODE_MEDIA;
    applyContrast();
    drawMediaScreen();
}

void tickMedia()
{
    if (gMode != MODE_MEDIA)
        return;
    if (gOsdType != OSD_NONE)
        return;
    uint32_t now = millis();
    if (now - gMediaLastMs >= MEDIA_SCROLL_SPEED_MS) {
        gMediaLastMs = now;
        gMediaScroll++;
        drawMediaScreen();
    }
}

// Разбирает "ЧЧ:ММ[:СС]" и обновляет внутренние часы.
bool applyClock(const char* s, bool withDate)
{
    int h = 0, m = 0, sec = 0;
    int n = sscanf(s, "%d:%d:%d", &h, &m, &sec);
    if (n < 2)
        return false;
    if (n == 2)
        sec = 0;
    h %= 24;
    m %= 60;
    sec %= 60;
    if (h < 0)
        h += 24;
    if (m < 0)
        m += 60;
    if (sec < 0)
        sec += 60;
    gClockSec = h * 3600 + m * 60 + sec;
    gClockSetMs = millis();
    gClockSet = true;

    // Дата идёт после времени: "ДЕНЬ_НЕДЕЛИ, Д МЕСЯЦ".
    if (withDate) {
        const char* q = s;
        while (*q && *q != ' ')
            q++;
        while (*q == ' ')
            q++;
        if (*q)
            snprintf(gDate, sizeof(gDate), "%s", q);
    }
    return true;
}

int findVal(const char* tag, const char* data)
{
    const char* p = strstr(data, tag);
    if (!p)
        return -1;
    p += strlen(tag);
    while (*p == ' ')
        p++;
    return atoi(p);
}

void processLine(const char* line)
{
    gLastData = millis();

    int v;

    if (strncmp(line, "NOWPLAY", 7) == 0) {
        const char* p = line + 7;
        while (*p == ' ')
            p++;
        char icon[4] = "0";
        int ic = 0;
        while (*p && *p != ' ' && ic < (int)sizeof(icon) - 1)
            icon[ic++] = *p++;
        icon[ic] = 0;
        while (*p == ' ')
            p++;
        setMedia(icon, p);
        return;
    }

    if ((v = findVal("BRIGHT", line)) >= 0) {
        setOsd(OSD_BRIGHT, v);
        return;
    }
    if (strncmp(line, "STARS", 5) == 0) {
        gMode = MODE_STARS;
        applyContrast();
        drawStars();
        return;
    }
    if (strncmp(line, "KEY", 3) == 0) {
        if (gMode == MODE_STARS) {
            starBurst();
            drawStars();
        }
        return;
    }
    if ((v = findVal("KBD", line)) >= 0) {
        gKbd = v;
        setOsd(OSD_KBD, v);
        applyContrast();
        return;
    }
    if ((v = findVal("VOL", line)) >= 0) {
        int mute = findVal("MUTE", line);
        if (mute >= 0)
            gMuted = mute;
        setOsd(OSD_VOL, v);
        return;
    }
    if ((v = findVal("MEDIA", line)) >= 0) {
        setOsd(OSD_MEDIA, v);
        return;
    }
    int mute = findVal("MUTE", line);
    if (mute >= 0) {
        gMuted = mute;
        setOsd(OSD_VOL, gOsdVal);
        return;
    }

    // CLK обновляет время, но не переключает режим экрана, поэтому
    // строка метрик может содержать и время, и CPU/RAM/DISK/TEMP.
    const char* clk = strstr(line, "CLK");
    if (clk) {
        clk += 3;
        while (*clk == ' ')
            clk++;
        applyClock(clk, true);
    }

    const char* p = strstr(line, "TIME");
    if (p) {
        p += 4;
        while (*p == ' ')
            p++;
        if (applyClock(p, true)) {
            gMode = MODE_CLOCK;
            applyContrast();
            drawClock();
        }
        return;
    }

    int cpu = findVal("CPU", line);
    int ram = findVal("RAM", line);
    int disk = findVal("DISK", line);
    int temp = findVal("TEMP", line);

    if (cpu >= 0)
        gCpu = cpu;
    if (ram >= 0)
        gRam = ram;
    if (disk >= 0)
        gDisk = disk;
    if (temp >= 0)
        gTemp = temp;

    if (cpu >= 0 || ram >= 0 || disk >= 0 || temp >= 0) {
        gMode = MODE_METRICS;
        applyContrast();
        drawMetrics();
        return;
    }

    // Строка только со временем: обновляем текущий экран, не меняя режим.
    if (clk && gClockSet) {
        switch (gMode) {
            case MODE_CLOCK: drawClock(); break;
            case MODE_STARS: drawStars(); break;
            case MODE_MEDIA: drawMediaScreen(); break;
            default: drawMetrics(); break;
        }
    }
}

class BLEHandler : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic* c) override
    {
        std::string v = c->getValue();
        if (v.length() == 0)
            return;
        char item[BLEQ_ITEM_SIZE];
        int n = v.length();
        if (n >= (int)sizeof(item))
            n = sizeof(item) - 1;
        memcpy(item, v.data(), n);
        item[n] = 0;
        gBleLastData = millis();
        if (xQueueSend(bleQueue, item, 0) != pdTRUE) {
            xQueueReset(bleQueue);
            xQueueSend(bleQueue, item, 0);
        }
    }
};

static volatile bool gBleConnected = false;

class BLEServerCb : public BLEServerCallbacks
{
    void onConnect(BLEServer* srv) override
    {
        gBleConnected = true;
        gBleLastData = millis();
    }
    void onDisconnect(BLEServer* srv) override
    {
        gBleConnected = false;
        delay(200);
        BLEDevice::startAdvertising();
        Serial.println("BLE: re-advertising");
    }
};

BLEServer* gServer = nullptr;

void startBLE()
{
    BLEDevice::init(BLE_NAME);
    gServer = BLEDevice::createServer();
    gServer->setCallbacks(new BLEServerCb());
    BLEService* svc = gServer->createService(BLE_SERVICE);
    BLECharacteristic* rx = svc->createCharacteristic(
        BLE_CHAR_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    BLECharacteristic* tx = svc->createCharacteristic(
        BLE_CHAR_TX, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    rx->setCallbacks(new BLEHandler());
    svc->start();

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMaxPreferred(0x12);
    BLEDevice::startAdvertising();
}

void setup()
{
    bleQueue = xQueueCreate(BLEQ_ITEMS, BLEQ_ITEM_SIZE);

    u8g2.begin();
    u8g2.setContrast(180);
    drawMetrics();

    Serial.begin(115200);
    Serial.println("OLED monitor ready");
    startBLE();
    Serial.println("BLE online");
}

void loop()
{
    static char line[160];
    static int n = 0;

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            line[n] = 0;
            n = 0;
            processLine(line);
            Serial.println("OK");
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = c;
        }
    }

    char b[BLEQ_ITEM_SIZE];
    while (xQueueReceive(bleQueue, b, 0) == pdTRUE) {
        processLine(b);
    }

    uint32_t now = millis();
    if (gBleConnected && gBleLastData != 0 &&
        (now - gBleLastData) >= BLE_STALE_MS) {
        // macOS может потерять BLE-сессию во время сна без disconnect
        // callback на стороне ESP32. Сбрасываем ложное состояние connected
        // и возвращаем периодическое рекламное объявление.
        gBleConnected = false;
        if (gServer != nullptr && gServer->getConnectedCount() > 0) {
            gServer->disconnect(gServer->getConnId());
        }
        BLEDevice::startAdvertising();
        Serial.println("BLE: stale connection, re-advertising");
    }
    if (gOsdType != OSD_NONE && (now - gOsdUntil) >= OSD_DURATION) {
        gOsdType = OSD_NONE;
        if (gMode == MODE_CLOCK)
            drawClock();
        else if (gMode == MODE_STARS)
            drawStars();
        else if (gMode == MODE_MEDIA)
            drawMediaScreen();
        else
            drawMetrics();
    }

    if (gClockSet && (now - gLastData) > DATA_TIMEOUT_MS && gMode != MODE_CLOCK) {
        gMode = MODE_CLOCK;
        applyContrast();
        drawClock();
    }

    tickStars();
    tickMedia();
    tickPixelShift();

    static int8_t drawnMin = -1;
    if (gMode == MODE_CLOCK && gClockSet) {
        long secElapsed = (long)((now - gClockSetMs) / 1000);
        int curMin = (int)((gClockSec + secElapsed) / 60);
        if (curMin != drawnMin) {
            drawnMin = curMin;
            drawClock();
        }
    }

    static uint32_t lastAdv = 0;
    if (lastAdv == 0)
        lastAdv = now;
    if (!gBleConnected && now - lastAdv >= BLE_ADV_RETRY_MS) {
        lastAdv = now;
        BLEDevice::startAdvertising();
        Serial.println("BLE: keepalive adv");
    }
}