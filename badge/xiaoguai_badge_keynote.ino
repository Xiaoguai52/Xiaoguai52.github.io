/*
 * 小乖桌面值班牌 · Keynote 版 v2.0（2026-11-08）
 * 由正式版 v1.0（xiaoguai_badge.ino）整文件派生：硬件层 / 按键 / WiFi / feed / NTP / 轮播逻辑保持等价，
 * 只重写视觉层 —— 午夜紫「苹果发布会」设计系统（纯黑底 + 白字 + 发丝线 + 紫罗兰强调），
 * 新增：数字页（本地算天数）、倒计时页 HH:MM:SS 逐秒走字、11 月 9 日当天整页翻转为生日页。
 * 硬件：正点原子 ATK-DNESP32S3-BOX（ESP32-S3 N16R8）
 * 引脚依据 xiaozhi-esp32 板级定义（alientek/atk-dnesp32s3-box）：
 *   LCD ST7789 320x240 i80：D0..D7=11,12,13,14,21,8,9,10 WR=4 RD=5 DC=3 CS=6 RST=2
 *   I2C：SDA=48 SCL=45；XL9555@0x20（P0.5/P0.7 拉高=功放/背光；K1=P0.4 K2=P0.3 输入，低有效）
 *   K0 = GPIO0（低有效）
 * 功能：6 页轮播（开场/倒计时/数字/天气/特斯拉/简报）+ 抽签页（K0 长按，不进轮播）、WiFi 拉 feed、NTP 对时、按键翻页
 * 依赖库：LovyanGFX、ArduinoJson
 * ⚠️ 烧录前：确认出厂固件备份 atk-box-factory.bin 已完整读出（16,777,216 字节）
 */

// ============ CONFIG（烧录前在本机改这两处） ============
#define WIFI_SSID  "在这里填家里WiFi名"
#define WIFI_PASS  "在这里填家里WiFi密码"
#define FEED_URL   "https://www.teslaxg.top/badge/feed.json"
// ======================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include <time.h>

// ---------- 显示设备（i80 ST7789） ----------
class LGFX_BOX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_Parallel8 _bus;
public:
  LGFX_BOX() {
    auto bcfg = _bus.config();
    bcfg.freq_write = 10000000;
    bcfg.pin_wr = 4;  bcfg.pin_rd = 5;  bcfg.pin_rs = 3;
    bcfg.pin_d0 = 11; bcfg.pin_d1 = 12; bcfg.pin_d2 = 13; bcfg.pin_d3 = 14;
    bcfg.pin_d4 = 21; bcfg.pin_d5 = 8;  bcfg.pin_d6 = 9;  bcfg.pin_d7 = 10;
    _bus.config(bcfg);
    _panel.setBus(&_bus);
    auto pcfg = _panel.config();
    pcfg.pin_cs = 6; pcfg.pin_rst = 2; pcfg.pin_busy = -1;
    pcfg.memory_width = 240; pcfg.memory_height = 320;
    pcfg.panel_width = 240;  pcfg.panel_height = 320;
    pcfg.invert = false; pcfg.rgb_order = false;
    _panel.config(pcfg);
    setPanel(&_panel);
  }
};
LGFX_BOX tft;

// ---------- 配色（Keynote 设计系统 + 小乖脸的固有色） ----------
static const uint32_t BG = 0x000000, WHITE = 0xFFFFFF, GRAY = 0x86868B,
                      HAIR = 0x2A2A2E, VIOLET = 0x7B2FF7, LILAC = 0xB18CFF,
                      DARK = 0x1C1C1E;
static const uint32_t CREAM = 0xFFF7EA, TEAL = 0x19B8C9, INK = 0x2B3245,
                      PINK = 0xFFB7C5;

// ---------- XL9555 ----------
#define XL_ADDR 0x20
uint8_t xlOut = 0;
void xlWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(XL_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
uint8_t xlRead(uint8_t reg) {
  Wire.beginTransmission(XL_ADDR); Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)XL_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}
void xlInit() {
  Wire.begin(48, 45);
  xlOut = (1 << 5) | (1 << 7);                 // 功放 + 背光
  xlWrite(0x06, 0xFF & ~((1 << 5) | (1 << 7))); // P0.5/P0.7 输出，其余输入（按键）
  xlWrite(0x02, xlOut);
}
bool keyK1() { return !(xlRead(0x00) & (1 << 4)); }   // 低有效
bool keyK2() { return !(xlRead(0x00) & (1 << 3)); }
bool keyK0() { return digitalRead(0) == LOW; }

// ---------- 数据 ----------
struct Feed {
  String cond = "阵雨"; int tmin = 24, tmax = 30; String tip = "出门记得带伞呀";
  String brief[3] = {"正在等小乖喂数据…", "", ""};
  int batt = 88, rangeKm = 394; bool locked = true; String place = "南宁 · 已停稳";
  String note = "小乖在值班台看着你呢";
} feed;

const char* FORTUNES[7] = {
  "大吉|今天会有好事敲门，记得开门哦",
  "中吉|适合把拖了很久的小事办掉",
  "小吉|出门会遇见让你笑一下的事",
  "吉|慢慢来，今天的节奏刚刚好",
  "中吉|宜喝热饮，宜早一点回家",
  "小吉|有个小惊喜，正在路上啦",
  "大吉|心里想的那件事，有戏"
};
int fortuneIdx = -1;

// ---------- 小乖的脸（画法沿用 v1.0） ----------
void drawGuaiFace(int cx, int cy, int r, bool blink) {
  for (int i = 0; i < 14; i++) {
    float a = i * 6.28318f / 14;
    int x = cx + cos(a) * r * 0.92, y = cy + sin(a) * r * 0.92;
    tft.fillCircle(x, y, r * 0.34, CREAM);
    tft.drawCircle(x, y, r * 0.34, 0xF0DFC2);
  }
  tft.fillCircle(cx, cy, r, CREAM);
  int gw = r * 0.52, gh = r * 0.40, gy = cy - r * 0.95;
  tft.fillRoundRect(cx - gw - 3, gy, gw, gh, gh / 2, TEAL);
  tft.fillRoundRect(cx + 3, gy, gw, gh, gh / 2, TEAL);
  tft.fillCircle(cx - gw / 2 - 3, gy + gh / 2, gh * 0.30, INK);
  tft.fillCircle(cx + gw / 2 + 3, gy + gh / 2, gh * 0.30, INK);
  int ey = cy + r * 0.08, ex = r * 0.36;
  if (blink) {
    tft.drawLine(cx - ex - 5, ey, cx - ex + 5, ey, INK);
    tft.drawLine(cx + ex - 5, ey, cx + ex + 5, ey, INK);
  } else {
    tft.fillCircle(cx - ex, ey, r * 0.09, INK);
    tft.fillCircle(cx + ex, ey, r * 0.09, INK);
  }
  tft.fillCircle(cx - r * 0.52, cy + r * 0.34, r * 0.15, PINK);
  tft.fillCircle(cx + r * 0.52, cy + r * 0.34, r * 0.15, PINK);
  tft.drawArc(cx, cy + r * 0.22, r * 0.16, r * 0.13, 20, 160, INK);
}

// ---------- 通用小组件 ----------
String two(int v) { return v < 10 ? "0" + String(v) : String(v); }
// 用当前字体/字号/颜色，把 s 横向居中打在 y 行（返回文字宽度）
int printC(const String& s, int y) {
  int w = tft.textWidth(s.c_str());
  tft.setCursor((320 - w) / 2, y); tft.print(s);
  return w;
}
// 左对齐眉题（小灰字，页眉统一位置）
void eyebrow(const char* t) {
  tft.setFont(&fonts::efontCN_16); tft.setTextSize(1); tft.setTextColor(GRAY);
  tft.setCursor(24, 22); tft.print(t);
}

// ---------- 日期计算（本地算，不依赖 feed） ----------
int daysUntilNext(int mon, int day, const struct tm& now) {
  struct tm t = now; t.tm_mon = mon - 1; t.tm_mday = day; t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
  time_t target = mktime(&t);
  struct tm n = now; n.tm_hour = 0; n.tm_min = 0; n.tm_sec = 0;
  time_t today = mktime(&n);
  if (target < today) { t.tm_year += 1; target = mktime(&t); }
  return (int)((target - today) / 86400);
}
// 距下一个 11 月 9 日 00:00 的剩余秒数（顺带给出目标年份）
long secsToBirthday(const struct tm& now, int& targetYear) {
  struct tm t = now; t.tm_mon = 10; t.tm_mday = 9; t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
  time_t target = mktime(&t);
  struct tm n = now; time_t cur = mktime(&n);
  if (target <= cur) { t.tm_year += 1; target = mktime(&t); }
  targetYear = t.tm_year + 1900;
  return (long)(target - cur);
}
// 2020-12-11 到今天的天数（在一起的天数）
int daysTogether(const struct tm& now) {
  struct tm start; memset(&start, 0, sizeof(start));
  start.tm_year = 2020 - 1900; start.tm_mon = 11; start.tm_mday = 11;
  struct tm n = now; n.tm_hour = 0; n.tm_min = 0; n.tm_sec = 0;
  time_t a = mktime(&start), b = mktime(&n);
  return (int)((b - a) / 86400);
}

// ---------- 页面 ----------
enum Page { P_FACE, P_COUNT, P_STATS, P_WEATHER, P_TESLA, P_BRIEF, P_FORTUNE, P_NUM };
int page = P_FACE;
bool blinkState = false;

// 1 · 开场页（兼时钟待机，眨眼沿用）
void drawFace() {
  tft.fillScreen(BG);
  struct tm now; getLocalTime(&now);
  // 眉题（紫）
  tft.setFont(&fonts::efontCN_16); tft.setTextSize(1); tft.setTextColor(VIOLET);
  tft.setCursor(24, 24); tft.print("XIAOGUAI KEYNOTE · 11.09");
  // 赖大宝（白大字）
  tft.setFont(&fonts::efontCN_24); tft.setTextSize(2); tft.setTextColor(WHITE);
  tft.setCursor(22, 46); tft.print("赖大宝");
  // Pro（白）+ 26（紫）双色大字
  tft.setTextColor(WHITE); tft.setCursor(24, 110); tft.print("Pro");
  int pw = tft.textWidth("Pro");
  tft.setTextColor(VIOLET); tft.setCursor(24 + pw + 10, 110); tft.print("26");
  tft.setTextSize(1);
  // 时钟一行（灰）：HH:MM · 日期星期
  const char* wk[] = {"日", "一", "二", "三", "四", "五", "六"};
  tft.setFont(&fonts::efontCN_16); tft.setTextColor(GRAY);
  tft.setCursor(24, 172);
  tft.printf("%s:%s · %d月%d日 · 星期%s", two(now.tm_hour).c_str(), two(now.tm_min).c_str(),
             now.tm_mon + 1, now.tm_mday, wk[now.tm_wday]);
  // 发丝线 + 底注
  tft.drawLine(24, 198, 296, 198, HAIR);
  tft.setCursor(24, 208); tft.print("南宁 · 生日晚");
  if (WiFi.status() != WL_CONNECTED) {
    int w = tft.textWidth("离线模式");
    tft.setCursor(320 - w - 24, 208); tft.print("离线模式");
  }
  // 右侧小乖的脸（黑底奶油脸）
  drawGuaiFace(238, 118, 44, blinkState);
}

// 2 · 倒计时页（核心创意：11 月 9 日当天翻转为生日页）
void drawCount() {
  tft.fillScreen(BG);
  struct tm now; getLocalTime(&now);
  if (now.tm_mon == 10 && now.tm_mday == 9) {
    // —— 生日当天 ——
    drawGuaiFace(160, 58, 26, blinkState);
    tft.setFont(&fonts::efontCN_24); tft.setTextSize(2); tft.setTextColor(WHITE);
    printC("生日快乐", 104);
    tft.setTextColor(VIOLET);
    printC("赖大宝", 164);
    tft.setTextSize(1);
    return;
  }
  int ty = 0; long rem = secsToBirthday(now, ty);
  int hh = (int)(rem / 3600), mm = (int)((rem % 3600) / 60), ss = (int)(rem % 60);
  // 眉题（居中灰字，岁数按目标年份算）
  tft.setFont(&fonts::efontCN_16); tft.setTextSize(1); tft.setTextColor(GRAY);
  printC("距 " + String(ty - 2000) + " 岁生日", 28);
  // 巨型 HH:MM:SS：数字白、冒号紫，分段画 + 实测宽度居中
  String sH = two(hh), sM = two(mm), sS = two(ss);
  if (hh >= 100) sH = String(hh);
  tft.setFont(&fonts::Font7); tft.setTextColor(WHITE);
  float ts = 2.0f; tft.setTextSize(ts);
  int wH = tft.textWidth(sH.c_str()), wM = tft.textWidth(sM.c_str()), wS = tft.textWidth(sS.c_str());
  int wC = tft.textWidth(":");
  int total = wH + wM + wS + 2 * wC;
  if (total > 312) { ts = 1.5f; tft.setTextSize(ts);
    wH = tft.textWidth(sH.c_str()); wM = tft.textWidth(sM.c_str()); wS = tft.textWidth(sS.c_str());
    wC = tft.textWidth(":"); total = wH + wM + wS + 2 * wC; }
  int x = (320 - total) / 2, y = 84;
  tft.setTextColor(WHITE); tft.setCursor(x, y);        tft.print(sH); int cH = x + wH / 2; x += wH;
  tft.setTextColor(VIOLET); tft.setCursor(x, y);       tft.print(":"); x += wC;
  tft.setTextColor(WHITE); tft.setCursor(x, y);        tft.print(sM); int cM = x + wM / 2; x += wM;
  tft.setTextColor(VIOLET); tft.setCursor(x, y);       tft.print(":"); x += wC;
  tft.setTextColor(WHITE); tft.setCursor(x, y);        tft.print(sS);
  tft.setTextSize(1);
  // 时 / 分 / 秒 微型标签（与各段中心对齐）
  tft.setFont(&fonts::efontCN_16); tft.setTextColor(GRAY);
  const char* lab[3] = {"时", "分", "秒"}; int ctr[3] = {cH, cM, x + wS / 2};
  for (int i = 0; i < 3; i++) {
    int w = tft.textWidth(lab[i]);
    tft.setCursor(ctr[i] - w / 2, 164); tft.print(lab[i]);
  }
  // 底部目标日期（灰）
  printC(String(ty) + ".11.09 · 00:00", 208);
}

// 3 · 数字页（苹果参数页语法，三列）
void drawStats() {
  tft.fillScreen(BG); eyebrow("关键数字");
  struct tm now; getLocalTime(&now);
  int days = daysTogether(now);
  int age = now.tm_year + 1900 - 2000;   // 她今年满的岁数
  String v1 = String(days), v2 = "5", v3 = String(age);
  int ctr[3] = {59, 170, 271};
  // 列间发丝竖线（只到数值区）
  tft.drawLine(118, 58, 118, 140, HAIR);
  tft.drawLine(222, 58, 222, 140, HAIR);
  // 数值（白，同号）
  tft.setFont(&fonts::Font7); tft.setTextSize(1.2f); tft.setTextColor(WHITE);
  String vs[3] = {v1, v2, v3};
  for (int i = 0; i < 3; i++) {
    int w = tft.textWidth(vs[i].c_str());
    tft.setCursor(ctr[i] - w / 2, 76); tft.print(vs[i]);
  }
  tft.setTextSize(1);
  // caption（灰）
  tft.setFont(&fonts::efontCN_16); tft.setTextColor(GRAY);
  const char* cap[3] = {"天 · 在一起", "场 · 一起听的", "岁 · 今年的她"};
  for (int i = 0; i < 3; i++) {
    int w = tft.textWidth(cap[i]);
    tft.setCursor(ctr[i] - w / 2, 150); tft.print(cap[i]);
  }
  int w = tft.textWidth("演唱会");
  tft.setCursor(ctr[1] - w / 2, 174); tft.print("演唱会");
}

// 4 · 天气页
void drawWeather() {
  tft.fillScreen(BG); eyebrow("南宁 · 天气");
  // 巨号最高温（白）+ / 低（灰小字）
  tft.setFont(&fonts::efontCN_24); tft.setTextSize(3); tft.setTextColor(WHITE);
  String t = String(feed.tmax) + "°";
  tft.setCursor(24, 52); tft.print(t);
  int tw = tft.textWidth(t.c_str());
  tft.setTextSize(1);
  tft.setFont(&fonts::efontCN_16); tft.setTextColor(GRAY);
  tft.setCursor(24 + tw + 10, 108); tft.printf("/ 低 %d°", feed.tmin);
  // 状况中文大字（白）
  tft.setFont(&fonts::efontCN_24); tft.setTextSize(2); tft.setTextColor(WHITE);
  tft.setCursor(24, 144); tft.print(feed.cond);
  tft.setTextSize(1);
  // AQI 药丸（深灰底白字）
  tft.setFont(&fonts::efontCN_16);
  String aqi = "空气 良";
  int pw = tft.textWidth(aqi.c_str()) + 24;
  tft.fillRoundRect(320 - pw - 24, 152, pw, 28, 14, DARK);
  tft.setTextColor(WHITE); tft.setCursor(320 - pw - 24 + 12, 158); tft.print(aqi);
  // tip（底部灰字）
  tft.setTextColor(GRAY); tft.setCursor(24, 210); tft.print(feed.tip);
}

// 5 · 特斯拉页
void drawTesla() {
  tft.fillScreen(BG); eyebrow("我的特斯拉");
  tft.setFont(&fonts::Font7); tft.setTextSize(3); tft.setTextColor(WHITE);
  tft.setCursor(24, 50); tft.printf("%d%%", feed.batt);
  tft.setTextSize(1);
  // 电池条：深灰轨道 + 白色填充（充满末端一段紫）
  int bw = 272, fw = (int)(bw * feed.batt / 100.0);
  tft.fillRoundRect(24, 164, bw, 10, 5, DARK);
  if (fw > 10) {
    tft.fillRoundRect(24, 164, fw, 10, 5, WHITE);
    int cap = fw > 26 ? 14 : fw / 2;
    tft.fillRoundRect(24 + fw - cap, 164, cap, 10, 5, LILAC);
  }
  tft.setFont(&fonts::efontCN_16); tft.setTextColor(GRAY);
  tft.setCursor(24, 188); tft.printf("约 %d km", feed.rangeKm);
  tft.setCursor(24, 212);
  tft.print(feed.place + (feed.locked ? " · 车已锁好" : " · 还没锁车"));
}

// 6 · 简报页（发布会议程语法）
void drawBrief() {
  tft.fillScreen(BG); eyebrow("今日简报");
  tft.drawLine(24, 46, 296, 46, HAIR);
  for (int i = 0; i < 3; i++) {
    int y = 62 + i * 48;
    tft.setFont(&fonts::Font7); tft.setTextSize(1); tft.setTextColor(VIOLET);
    tft.setCursor(24, y); tft.printf("0%d", i + 1);
    tft.setFont(&fonts::efontCN_16); tft.setTextColor(WHITE);
    tft.setCursor(76, y + 6);
    if (feed.brief[i].length()) tft.print(feed.brief[i]);
    if (i < 2) tft.drawLine(24, y + 34, 296, y + 34, HAIR);
  }
  tft.drawLine(24, 196, 296, 196, HAIR);
  tft.setFont(&fonts::efontCN_16); tft.setTextColor(GRAY);
  tft.setCursor(24, 206); tft.print(feed.note);
}

// 7 · 抽签页（K0 长按触发，不进轮播）
void drawFortune() {
  tft.fillScreen(BG);
  if (fortuneIdx < 0) fortuneIdx = esp_random() % 7;
  String s = FORTUNES[fortuneIdx];
  int bar = s.indexOf('|');
  tft.setFont(&fonts::efontCN_24); tft.setTextSize(1); tft.setTextColor(WHITE);
  printC("One more thing.", 30);
  // 深灰卡 + 紫色描边
  tft.fillRoundRect(24, 82, 272, 108, 16, DARK);
  tft.drawRoundRect(24, 82, 272, 108, 16, VIOLET);
  tft.setFont(&fonts::efontCN_24); tft.setTextColor(VIOLET);
  tft.setCursor(44, 98); tft.print(s.substring(0, bar));
  tft.setFont(&fonts::efontCN_16); tft.setTextColor(WHITE);
  tft.setCursor(44, 142); tft.print(s.substring(bar + 1));
  tft.setTextColor(GRAY);
  printC("按任意键回轮播 · 长按 K0 再抽一支", 206);
}
void drawPage() {
  switch (page) {
    case P_FACE: drawFace(); break;
    case P_COUNT: drawCount(); break;
    case P_STATS: drawStats(); break;
    case P_WEATHER: drawWeather(); break;
    case P_TESLA: drawTesla(); break;
    case P_BRIEF: drawBrief(); break;
    case P_FORTUNE: drawFortune(); break;
  }
}

// ---------- 网络 ----------
void fetchFeed() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http; http.setTimeout(5000); http.begin(FEED_URL);
  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      feed.cond = doc["weather"]["cond"] | feed.cond;
      feed.tmin = doc["weather"]["tmin"] | feed.tmin;
      feed.tmax = doc["weather"]["tmax"] | feed.tmax;
      feed.tip  = doc["weather"]["tip"]  | feed.tip;
      for (int i = 0; i < 3; i++) feed.brief[i] = doc["briefing"][i] | feed.brief[i];
      feed.batt = doc["tesla"]["battery"] | feed.batt;
      feed.rangeKm = doc["tesla"]["range_km"] | feed.rangeKm;
      feed.locked  = doc["tesla"]["locked"] | feed.locked;
      feed.place   = doc["tesla"]["place"] | feed.place;
      feed.note    = doc["note"] | feed.note;
    }
  }
  http.end();
}
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() == WL_CONNECTED) configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
}

// ---------- 主流程 ----------
unsigned long lastPage = 0, lastFetch = 0, lastBlink = 0, lastTick = 0, k0Down = 0;
bool k0Was = false, k1Was = false, k2Was = false;
const unsigned long PAGE_MS = 8000, FETCH_MS = 5 * 60 * 1000;

void setup() {
  pinMode(0, INPUT_PULLUP);
  xlInit();
  tft.init(); tft.setRotation(1);
  tft.fillScreen(BG);
  drawGuaiFace(160, 108, 52, false);
  tft.setFont(&fonts::efontCN_16); tft.setTextSize(1); tft.setTextColor(WHITE);
  printC("小乖 · 正在上岗", 186);
  wifiConnect();
  fetchFeed();
  drawPage();
  lastPage = lastFetch = millis();
}

void loop() {
  unsigned long now = millis();
  bool a = keyK0(), b = keyK1(), c = keyK2();
  if (a && !k0Was) k0Down = now;
  bool anyPress = (a && !k0Was) || (b && !k1Was) || (c && !k2Was);
  if (!a && k0Was && k0Down && now - k0Down > 1200) {   // K0 长按松开 → 抽签
    fortuneIdx = esp_random() % 7; page = P_FORTUNE; drawPage(); lastPage = now; k0Down = 0;
  } else if (anyPress) {                                 // 短按 → 下一页（抽签页按任意键回开场）
    if (page == P_FORTUNE) page = P_FACE; else page = (page + 1) % P_NUM;
    drawPage(); lastPage = now;
  }
  k0Was = a; k1Was = b; k2Was = c;
  if (page != P_FORTUNE && now - lastPage > PAGE_MS) { page = (page + 1) % P_NUM; lastPage = now; drawPage(); }
  if (page == P_FACE && now - lastBlink > 2600) {        // 待机页眨眼 + 时钟走字
    lastBlink = now; blinkState = !blinkState;
    if (blinkState || (now / 1000) % 30 < 2) drawPage();
  }
  if (page == P_COUNT && now - lastTick >= 1000) {       // 倒计时页逐秒走字（零点自动翻生日页）
    lastTick = now; drawPage();
  }
  if (now - lastFetch > FETCH_MS) { lastFetch = now; if (WiFi.status() != WL_CONNECTED) wifiConnect(); fetchFeed(); }
  delay(30);
}
