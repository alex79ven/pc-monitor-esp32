#include <Arduino.h>
#include <HWCDC.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <math.h>

#ifndef OLED_SDA
#define OLED_SDA 8
#endif
#ifndef OLED_SCL
#define OLED_SCL 9
#endif

#ifndef BLE_NAME
#define BLE_NAME "OLED-MONITOR"
#endif

#define BLE_SERVICE "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_CHAR_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_CHAR_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

#define BLEQ_ITEMS 8
#define BLEQ_ITEM_SIZE 160

#define OSD_DURATION 5000
#define DATA_TIMEOUT_MS 10000
// Если хост уснул, ESP32 может не получить callback отключения. Считаем
// соединение зависшим, если от host давно не было ни одной команды.
#define BLE_STALE_MS 5000
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
#define MEDIA_SCROLL_SPEED_MS 110
static char gMediaText[MEDIA_TEXT_MAX] = "";
static int gMediaIcon = 0;      // 0 spotify, 1 youtube
static int gMediaScroll = 0;
static uint32_t gMediaLastMs = 0;

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

void drawBar(int baseline, int v)
{
    int x = 42, w = 28, barH = 7;
    u8g2.drawFrame(x, baseline - barH + 1, w, barH);
    int fw = (int)((long)(w - 2) * v / 100);
    if (fw > 0)
        u8g2.drawBox(x + 1, baseline - barH + 2, fw, barH - 2);
}

void drawRow(int baseline, const char* label, int value)
{
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(2, baseline, label);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", value);
    u8g2.drawStr(24, baseline, buf);
    drawBar(baseline, value);
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

void drawClock()
{
    updateClockTime();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_logisoso24_tn);
    int w = u8g2.getStrWidth(gTime);
    u8g2.drawStr((72 - w) / 2, 32, gTime);
    u8g2.sendBuffer();
}

void drawMetrics()
{
    u8g2.clearBuffer();
    if (gCpu < 0 && gRam < 0 && gDisk < 0) {
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(6, 20, "PC MONITOR");
        u8g2.drawStr(6, 32, "waiting...");
        u8g2.sendBuffer();
        return;
    }
    if (gCpu >= 0)
        drawRow(9, "CPU", gCpu);
    if (gRam >= 0)
        drawRow(19, "RAM", gRam);
    if (gDisk >= 0)
        drawRow(29, "DSK", gDisk);

    u8g2.setFont(u8g2_font_6x10_tf);
    char buf[24];
    if (gTemp > 0) {
        snprintf(buf, sizeof(buf), "TMP %d", gTemp);
        u8g2.drawStr(2, 39, buf);
        int tw = u8g2.getStrWidth(buf);
        u8g2.drawCircle(2 + tw + 2, 34, 1);
        u8g2.drawStr(2 + tw + 5, 39, "C");
    }
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
    int cx = 36, cy = 20;
    for (int i = 0; i < STARS_N; i++) {
        starZ[i] -= 0.03f;
        if (starZ[i] <= 0.0f) {
            starX[i] = randF(-1.0f, 1.0f);
            starY[i] = randF(-1.0f, 1.0f);
            starZ[i] = 1.0f;
        }
        int px = cx + (int)(starX[i] / starZ[i] * 20.0f);
        int py = cy + (int)(starY[i] / starZ[i] * 11.0f);
        if (px < 0 || px >= 72 || py < 0 || py >= 40)
            continue;
        if (starZ[i] < 0.3f)
            u8g2.drawBox(px - 1, py - 1, 2, 2);
        else
            u8g2.drawPixel(px, py);
    }
    for (int i = 0; i < STARS_BURST; i++) {
        if (burstLife[i] == 0)
            continue;
        int px = cx + (int)(burstX[i] / burstZ[i] * 20.0f);
        int py = cy + (int)(burstY[i] / burstZ[i] * 11.0f);
        if (px >= 0 && px < 72 && py >= 0 && py < 40)
            u8g2.drawBox(px - 1, py - 1, 3, 3);
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
    u8g2.drawDisc(cx, cy, 3);
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4.0f;
        int x0 = cx + (int)(cosf(a) * 5), y0 = cy + (int)(sinf(a) * 5);
        int x1 = cx + (int)(cosf(a) * 9), y1 = cy + (int)(sinf(a) * 9);
        u8g2.drawLine(x0, y0, x1, y1);
    }
}

void drawVolumeIcon(int x, int y, bool muted)
{
    u8g2.drawBox(x, y + 5, 4, 6);
    u8g2.drawTriangle(x + 4, y + 6, x + 10, y + 2, x + 10, y + 14);
    u8g2.drawCircle(x + 12, y + 8, 2);
    u8g2.drawCircle(x + 12, y + 8, 4);
    if (muted) {
        u8g2.drawLine(x + 10, y + 6, x + 16, y + 12);
        u8g2.drawLine(x + 16, y + 6, x + 10, y + 12);
    }
}

void drawKbdIcon(int x, int y)
{
    u8g2.drawFrame(x, y + 2, 15, 12);
    u8g2.drawBox(x + 2, y + 4, 2, 2);
    u8g2.drawBox(x + 5, y + 4, 2, 2);
    u8g2.drawBox(x + 8, y + 4, 2, 2);
    u8g2.drawBox(x + 11, y + 4, 2, 2);
    u8g2.drawBox(x + 2, y + 8, 4, 2);
    u8g2.drawBox(x + 7, y + 8, 5, 2);
}

void drawMediaIcon(int x, int y, int mode)
{
    switch (mode) {
        case 0: // pause
            u8g2.drawBox(x + 1, y + 1, 4, 13);
            u8g2.drawBox(x + 9, y + 1, 4, 13);
            break;
        case 1: // play
            u8g2.drawTriangle(x + 2, y + 1, x + 2, y + 14, x + 14, y + 8);
            break;
        case 2: // toggle
            u8g2.drawTriangle(x + 1, y + 3, x + 1, y + 12, x + 9, y + 8);
            u8g2.drawBox(x + 10, y + 3, 4, 10);
            break;
        case 3: // next
            u8g2.drawTriangle(x + 1, y + 3, x + 1, y + 12, x + 9, y + 8);
            u8g2.drawBox(x + 10, y + 3, 3, 10);
            break;
        case 4: // prev
            u8g2.drawBox(x + 1, y + 3, 3, 10);
            u8g2.drawTriangle(x + 5, y + 3, x + 5, y + 12, x + 13, y + 8);
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

    u8g2.setFont(u8g2_font_6x10_tf);
    int lw = u8g2.getStrWidth(label);
    u8g2.drawStr((72 - lw) / 2, 8, label);

    if (type == OSD_MEDIA) {
        drawMediaIcon(28, 13, val);
        u8g2.sendBuffer();
        return;
    }

    switch (type) {
        case OSD_BRIGHT: drawBrightIcon(4, 12); break;
        case OSD_KBD: drawKbdIcon(3, 12); break;
        case OSD_VOL: drawVolumeIcon(4, 12, gMuted); break;
        default: break;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", val);
    u8g2.setFont(u8g2_font_logisoso24_tn);
    int nw = u8g2.getStrWidth(buf);
    u8g2.drawStr(24, 33, buf);

    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.drawStr(26 + nw, 22, "%");

    u8g2.drawFrame(2, 35, 68, 4);
    int fw = (int)(66L * val / 100);
    if (fw > 0)
        u8g2.drawBox(3, 36, fw, 2);

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
    u8g2.drawCircle(x + 8, y + 8, 8);
    u8g2.drawDisc(x + 8, y + 8, 3);
}

void drawYoutubeIcon(int x, int y)
{
    u8g2.drawRFrame(x, y, 19, 13, 2);
    u8g2.drawTriangle(x + 5, y + 3, x + 5, y + 10, x + 13, y + 7);
}

void drawMediaScreen()
{
    u8g2.clearBuffer();
    if (gMediaIcon == 1)
        drawYoutubeIcon(27, 3);
    else
        drawSpotifyIcon(28, 3);

    u8g2.setFont(u8g2_font_5x8_t_cyrillic);
    int textW = u8g2.getUTF8Width(gMediaText);
    if (textW <= 72) {
        u8g2.drawUTF8((72 - textW) / 2, 36, gMediaText);
    } else {
        int full = textW + 72;
        int sx = 72 - (gMediaScroll % full);
        u8g2.drawUTF8(sx, 36, gMediaText);
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

    char* p = strstr(line, "TIME");
    if (p) {
        p += 4;
        while (*p == ' ')
            p++;
        int h = 0, m = 0, s = 0;
        int n = sscanf(p, "%d:%d:%d", &h, &m, &s);
        if (n >= 2) {
            if (n == 2)
                s = 0;
            h %= 24;
            m %= 60;
            s %= 60;
            if (h < 0)
                h += 24;
            if (m < 0)
                m += 60;
            if (s < 0)
                s += 60;
            gClockSec = h * 3600 + m * 60 + s;
            gClockSetMs = millis();
            gClockSet = true;
            gMode = MODE_CLOCK;
            applyContrast();
            drawClock();
            return;
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