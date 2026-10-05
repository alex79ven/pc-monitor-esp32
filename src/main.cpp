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
// Дата в UTF-8: кириллица занимает по 2 байта, поэтому буфер
// расширен, иначе длинное "сентября" обрезалось бы.
static char gDate[32] = "--.--";
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
static int gFanRpm = -1;
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
// Шаг прокрутки в миллисекундах: меньше — быстрее.
// Исходно было 110, затем 55, теперь 27 (x4 от исходного).
#define MEDIA_SCROLL_SPEED_MS 27
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
inline void dUTF8(int x, int y, const char* s) { u8g2.drawUTF8(x + gOffX, y, s); }
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

// Полосы укорочены на SHIFT_POS_MAX, иначе при горизонтальном сдвиге
// правый край уходил бы за границу экрана.
#define BAR_X 54
#define BAR_W (72 - SHIFT_POS_MAX)
#define BAR_H 10
// Шаг диагональной штриховки: меньше — плотнее.
#define HATCH_STEP 5

// Диагональная штриховка под 45° вместо сплошной заливки.
// Пиксель рисуется, если (x + y) кратно HATCH_STEP.
void drawHatch(int x, int y, int w, int h)
{
    for (int col = 0; col < w; col++) {
        for (int row = 0; row < h; row++) {
            if (((col + row) % HATCH_STEP) == 0)
                dPixel(x + col, y + row);
        }
    }
}

void drawBar(int x, int baseline, int w, int barH, int v)
{
    dFrame(x, baseline - barH + 1, w, barH);
    int fw = (int)((long)(w - 2) * v / 100);
    if (fw > 0)
        drawHatch(x + 1, baseline - barH + 2, fw, barH - 2);
}

void drawRow(int baseline, const char* label, int value)
{
    u8g2.setFont(u8g2_font_6x10_tf);
    dStr(2, baseline, label);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", value);
    dStr(24, baseline, buf);
    drawBar(BAR_X, baseline, BAR_W, BAR_H, value);
}

// Шкала оборотов вентилятора. Нижняя граница — реальный минимум
// этого Mac (F1Mn = 2000), верхняя — 6200, чуть выше фактического
// максимума датчика (F0Mx = 6156), чтобы полоса доходила почти
// до конца, но не упиралась в 100 % слишком рано.
#define FAN_MIN_RPM 2000
#define FAN_MAX_RPM 6200

// Строка вентилятора: та же полоса, но 1000..6000 RPM и подпись в оборотах.
void drawFanRow(int baseline, int rpm)
{
    int pct = (rpm - FAN_MIN_RPM) * 100 / (FAN_MAX_RPM - FAN_MIN_RPM);
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    u8g2.setFont(u8g2_font_6x10_tf);
    dStr(2, baseline, "FAN");
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", rpm);
    dStr(24, baseline, buf);
    drawBar(BAR_X, baseline, BAR_W, BAR_H, pct);
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

    // Жёлтый прогресс-бар остаётся сплошной заливкой: он горит лишь
// 5 секунд во время OSD, поэтому постоянной нагрузки на пиксели нет.
// Ширина уменьшена на SHIFT_POS_MAX по той же причине, что и у полос
// в синей области, — иначе правый край уходил бы за границу.
    const int w = OLED_WIDTH - 4 - SHIFT_POS_MAX;
    dFrame(2, 8, w, 7);
    int fw = (int)((long)(w - 4) * val / 100);
    if (fw > 0)
        dBox(3, 9, fw, 5);
}

void drawClock()
{
    updateClockTime();
    u8g2.clearBuffer();

    // Дата по-русски в жёлтой полосе: "Пн, 5 октября".
    // Кириллица есть только в *_t_cyrillic шрифтах; logisoso содержит
    // лишь цифры и знаки. 9x15_t_cyrillic — самый крупный, который
    // влезает в 128 px по этой строке (117 px).
    u8g2.setFont(u8g2_font_9x15_t_cyrillic);
    int dw = u8g2.getUTF8Width(gDate);
    dUTF8((OLED_WIDTH - dw) / 2, 13, gDate);

    // Заливные часы: 42 px — максимальный размер, при котором
    // "23:45" помещается в ширину 128 px и по высоте в синюю область.
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

    if (gCpu < 0 && gRam < 0 && gFanRpm < 0) {
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
    if (gFanRpm >= 0)
        drawFanRow(56, gFanRpm);

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
    // В строке метрик после даты идут CPU/RAM/TEMP/FAN, поэтому
    // обрезаем всё начиная с первого известного тега, иначе в gDate
    // попадало бы "ПН, 5 ОКТ CPU 24 RAM 63 ...".
    if (withDate) {
        static const char* kTags[] = {
            "CPU", "RAM", "DISK", "TEMP", "FAN", "MEDIA",
            "BRIGHT", "KBD", "VOL", "MUTE", "KEY", "STARS", "NOWPLAY",
        };
        const char* q = s;
        while (*q && *q != ' ')
            q++;
        while (*q == ' ')
            q++;

        char date[sizeof(gDate)];
        size_t len = 0;
        while (q[len]) {
            bool cut = false;
            for (unsigned i = 0; i < sizeof(kTags) / sizeof(kTags[0]); i++) {
                size_t tl = strlen(kTags[i]);
                if (strncmp(q + len, kTags[i], tl) == 0) {
                    cut = true;
                    break;
                }
            }
            if (cut)
                break;
            len++;
        }
        while (len > 0 && q[len - 1] == ' ')
            len--;
        if (len > 0) {
            if (len >= sizeof(date))
                len = sizeof(date) - 1;
            memcpy(date, q, len);
            date[len] = 0;
            snprintf(gDate, sizeof(gDate), "%s", date);
        }
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
    int temp = findVal("TEMP", line);
    int fan = findVal("FAN", line);

    if (cpu >= 0)
        gCpu = cpu;
    if (ram >= 0)
        gRam = ram;
    if (temp >= 0)
        gTemp = temp;
    if (fan >= 0)
        gFanRpm = fan;

    if (cpu >= 0 || ram >= 0 || temp >= 0 || fan >= 0) {
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

// Источник истины о наличии клиента — собственный счётчик, а НЕ
// gServer->getConnectedCount(). У того есть баг в Arduino ESP32 BLE:
// счётчик инкрементируется в CONNECT_EVT, а декремент в DISCONNECT_EVT
// зависит от removePeerDevice(), который возвращает результат удаления из
// m_connectedServersMap. При рассинхроне счётчик залипает навсегда, и
// реклама больше не возобновляется — устройство пропадает из эфира уже
// после первого disconnect. Свой счётчик мы ведём сами.
static volatile int gBleClients = 0;

BLEServer* gServer = nullptr;

static inline bool bleHasClient()
{
    return gBleClients > 0;
}

class BLEServerCb : public BLEServerCallbacks
{
    void onConnect(BLEServer* srv) override
    {
        gBleClients++;
        gBleLastData = millis();
        Serial.printf("BLE: client connected (clients=%d)\n", gBleClients);
    }
    void onDisconnect(BLEServer* srv) override
    {
        // Здесь раньше стояли delay(200) и BLEDevice::startAdvertising().
        // Оба делали то же, что и ветка рекламы в loop(), поэтому реклама
        // стартовала дважды подряд на активном соединении, а delay()
        // блокировал BLE-стек прямо внутри callback. Оставляем только
        // счётчик: рекламу возобновит loop().
        if (gBleClients > 0)
            gBleClients--;
        gBleLastData = 0;
        Serial.printf("BLE: disconnected (clients=%d)\n", gBleClients);
    }
};

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
    // Сравнение ОБЯЗАТЕЛЬНО знаковое. gBleLastData ставится в BLE-задаче
    // (onWrite/onConnect), а now читается здесь, в задаче loop(). Между
    // чтениями проходит около 1 мс, поэтому метка регулярно оказывается
    // на 1 мс ВПЕРЕДИ now. Беззнаковая разность в uint32_t даёт
    // 13084 - 13085 = 0xFFFFFFFF (4294967 с), условие выполняется, и
    // ESP32 разрывает живое соединение сразу после каждой записи —
    // отсюда обрывы каждые 0.0-3 с.
    if (bleHasClient() && gBleLastData != 0 &&
        (int32_t)(now - gBleLastData) >= (int32_t)BLE_STALE_MS) {
        // macOS может потерять BLE-сессию во время сна без disconnect
        // callback на стороне ESP32. Рекламу здесь НЕ возобновляем: этим
        // занимается ветка advertising в loop(). Счётчик сбрасываем сразу,
        // чтобы реклама возобновилась ещё до disconnect-callback.
        Serial.println("BLE: stale connection, dropping");
        if (gBleClients > 0)
            gBleClients--;
        if (gServer != nullptr && gServer->getConnectedCount() > 0) {
            gServer->disconnect(gServer->getConnId());
        }
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

    // Рекламу возобновляем ТОЛЬКО когда реально нет клиента и только если
    // она сейчас не идёт. BLEAdvertising::start() не проверяет состояние
    // и каждый раз зовёт esp_ble_gap_start_advertising(); повторный вызов
    // на уже идущей рекламе сбивает BLE-стек, и устройство перестаёт
    // появляться при сканировании.
    // Рекламу возобновляем по таймеру, когда клиента нет. Флаг gAdvRunning
    // здесь НЕ используется: он залипал после первого запуска, реклама
    // больше не возобновлялась и устройство пропадало из эфира. В логе видно
    // было одну строку "BLE: advertising" за все 12 секунд.
    static uint32_t lastAdv = 0;
    if (lastAdv == 0)
        lastAdv = now;
    if (bleHasClient()) {
        lastAdv = now;
    } else if ((uint32_t)(now - lastAdv) >= BLE_ADV_RETRY_MS) {
        lastAdv = now;
        BLEDevice::startAdvertising();
        Serial.println("BLE: advertising");
    }
}