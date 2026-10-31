#include "farm.h"

#include "app_config.h"
#include "hal.h"

namespace farm {

namespace {

const char *const SENSOR_NAMES[N_ADC] = {"ความชื้นดิน", "ความสว่าง", "อุณหภูมิ", "ระดับน้ำ"};
const char *const OUTPUT_NAMES[N_OUT] = {"ปั๊มน้ำ", "พัดลม", "ไฟปลูก", "เตือนภัย"};
constexpr char KEY_RULES[] = "rules1";  // เปลี่ยนชื่อ (เลขเวอร์ชัน) ถ้าแก้โครงสร้าง Rule
constexpr char KEY_AUTO[] = "farmauto";

struct Runtime {
    bool latch1 = false, latch2 = false;  // สถานะค้างของเงื่อนไข (hysteresis / กดสลับ)
    bool raw = false;                     // ผลของเงื่อนไขล้วน
    bool active = false;                  // หลังรวมช่วงค้าง
    uint32_t lastTrue = 0;
};

Rule gRules[MAX_RULES];
Runtime gRt[MAX_RULES];
bool gAuto = true;
float gPct[N_ADC] = {0, 0, 0, 0};
int8_t gDesired[N_OUT] = {-1, -1, -1, -1};  // สถานะที่กฎสั่งล่าสุด (-1 = ยังไม่เคยสั่ง)
bool gOver[N_OUT] = {false, false, false, false};
uint8_t gPrevSw = 0;
uint32_t gLastEval = 0;
uint32_t gEventSeq = 0;
char gEvent[64] = "พร้อมทำงาน";

void logEvent(int out, bool on, const char *by)
{
    snprintf(gEvent, sizeof(gEvent), "%s %s (%s)", OUTPUT_NAMES[out], on ? "เปิด" : "ปิด", by);
    gEventSeq++;
    Serial.printf("[FARM] OUT%d %s by %s\n", out, on ? "ON" : "OFF", by);
}

bool evalCond(const Cond &c, bool &latch, uint8_t sw, uint8_t edges)
{
    if (c.src >= SRC_ADC0 && c.src < SRC_ADC0 + N_ADC) {
        if (!hal::adsPresent()) {
            latch = false;
            return false;
        }
        const float pct = gPct[c.src - SRC_ADC0];
        const float v = c.value, h = app::RULE_HYST_PCT;
        if (c.op == ADC_GREATER) {
            if (pct > v) latch = true;
            else if (pct < v - h) latch = false;
        } else {
            if (pct < v) latch = true;
            else if (pct > v + h) latch = false;
        }
        return latch;
    }
    if (c.src >= SRC_SW0 && c.src < SRC_SW0 + N_SW) {
        if (!hal::ioPresent()) return false;
        const int n = c.src - SRC_SW0;
        const bool pressed = (sw >> n) & 1;
        if (c.op == SW_PRESSED) return pressed;
        if (c.op == SW_RELEASED) return !pressed;
        if ((edges >> n) & 1) latch = !latch;
        return latch;
    }
    return false;
}

void defaultRules()
{
    for (Rule &r : gRules) r = Rule();
    // 0: ดินแห้ง -> รดน้ำ (ค้างไว้ 5 วินาทีให้น้ำซึม)
    gRules[0] = {true, {SRC_ADC0 + 0, ADC_LESS, 40}, {}, COMB_NONE, 0, true, 5};
    // 1: ร้อน -> เปิดพัดลม
    gRules[1] = {true, {SRC_ADC0 + 2, ADC_GREATER, 70}, {}, COMB_NONE, 1, true, 3};
    // 2: แสงน้อย -> เปิดไฟปลูก
    gRules[2] = {true, {SRC_ADC0 + 1, ADC_LESS, 30}, {}, COMB_NONE, 2, true, 0};
    // 3: น้ำในถังต่ำ -> เตือนภัย
    gRules[3] = {true, {SRC_ADC0 + 3, ADC_LESS, 15}, {}, COMB_NONE, 3, true, 0};
}

}  // namespace

const char *sensorName(int ch)
{
    return SENSOR_NAMES[ch & 3];
}

const char *outputName(int out)
{
    return OUTPUT_NAMES[out & 3];
}

void begin()
{
    defaultRules();
    Preferences &p = hal::prefs();
    if (p.getBytesLength(KEY_RULES) == sizeof(gRules)) p.getBytes(KEY_RULES, gRules, sizeof(gRules));
    for (Rule &r : gRules) {  // กันค่าเพี้ยนจาก NVS
        r.out &= 3;
        if (r.c1.value > 100) r.c1.value = 100;
        if (r.c2.value > 100) r.c2.value = 100;
        if (r.combine > COMB_OR) r.combine = COMB_NONE;
        if (r.holdSec > 60) r.holdSec = 60;
    }
    gAuto = p.getBool(KEY_AUTO, true);
    Serial.printf("[FARM] mode=%s\n", gAuto ? "AUTO" : "MANUAL");
}

void tick(uint32_t now)
{
    hal::ioPoll(now);  // สวิตช์ต้องถูกอ่านต่อเนื่องแม้ UI ไม่ได้เปิดหน้า I/O
    if (now - gLastEval < 20) return;
    gLastEval = now;

    // ---- เซนเซอร์ -> % ของเต็มสเกล (กรองให้นิ่ง) ----
    if (hal::adsPresent()) {
        for (int ch = 0; ch < N_ADC; ch++) {
            const float target = constrain(hal::adsVolts(ch) / app::ADC_FULLSCALE_V * 100.0f, 0.0f, 100.0f);
            gPct[ch] += (target - gPct[ch]) * 0.25f;
        }
    }

    const uint8_t sw = hal::ioSwitches();
    const uint8_t edges = sw & static_cast<uint8_t>(~gPrevSw);
    gPrevSw = sw;

    // ---- ประเมินทุกกฎ (แม้โหมดมือ ก็ประเมินไว้ให้หน้ากฎแสดงสถานะ) ----
    for (int i = 0; i < MAX_RULES; i++) {
        const Rule &r = gRules[i];
        Runtime &rt = gRt[i];
        if (!r.enabled || r.c1.src == SRC_NONE) {
            rt.raw = rt.active = false;
            continue;
        }
        bool v = evalCond(r.c1, rt.latch1, sw, edges);
        if (r.combine != COMB_NONE && r.c2.src != SRC_NONE) {
            const bool v2 = evalCond(r.c2, rt.latch2, sw, edges);
            v = r.combine == COMB_AND ? (v && v2) : (v || v2);
        }
        rt.raw = v;
        if (v) rt.lastTrue = now == 0 ? 1 : now;
        rt.active = v || (r.holdSec > 0 && rt.lastTrue != 0 && now - rt.lastTrue < static_cast<uint32_t>(r.holdSec) * 1000UL);
    }

    if (!gAuto) return;

    // ---- สั่งอุปกรณ์เฉพาะ "ตอนสถานะที่กฎต้องการเปลี่ยน" เพื่อให้สั่งมือแทรกได้ ----
    for (int o = 0; o < N_OUT; o++) {
        bool any = false, want = false;
        for (int i = 0; i < MAX_RULES; i++) {
            const Rule &r = gRules[i];
            if (!r.enabled || r.c1.src == SRC_NONE || r.out != o) continue;
            any = true;
            want = want || (r.onWhenTrue ? gRt[i].active : !gRt[i].active);
        }
        if (!any) {
            gDesired[o] = -1;
            continue;
        }
        if (gDesired[o] != static_cast<int8_t>(want)) {
            gDesired[o] = want;
            gOver[o] = false;
            if (((hal::ioLeds() >> o) & 1) != want) {
                hal::ioSetLeds(want ? (hal::ioLeds() | (1u << o)) : (hal::ioLeds() & ~(1u << o)));
                logEvent(o, want, "กฎอัตโนมัติ");
            }
        }
    }
}

Rule &rule(int i)
{
    return gRules[constrain(i, 0, MAX_RULES - 1)];
}

void saveRules()
{
    hal::prefs().putBytes(KEY_RULES, gRules, sizeof(gRules));
    for (int o = 0; o < N_OUT; o++) gDesired[o] = -1;  // ให้ประเมินใหม่ทั้งหมด
    for (Runtime &rt : gRt) rt = Runtime();
}

void resetRules()
{
    defaultRules();
    saveRules();
}

void clearRule(int i)
{
    gRules[constrain(i, 0, MAX_RULES - 1)] = Rule();
    saveRules();
}

bool autoMode()
{
    return gAuto;
}

void setAutoMode(bool on)
{
    gAuto = on;
    hal::prefs().putBool(KEY_AUTO, on);
    for (int o = 0; o < N_OUT; o++) {
        gDesired[o] = -1;
        gOver[o] = false;
    }
    Serial.printf("[FARM] mode=%s\n", on ? "AUTO" : "MANUAL");
}

float sensorPct(int ch)
{
    return gPct[ch & 3];
}

bool condNow(const Cond &c)
{
    if (c.src >= SRC_ADC0 && c.src < SRC_ADC0 + N_ADC) {
        if (!hal::adsPresent()) return false;
        const float pct = gPct[c.src - SRC_ADC0];
        return c.op == ADC_GREATER ? pct > c.value : pct < c.value;
    }
    if (c.src >= SRC_SW0 && c.src < SRC_SW0 + N_SW) {
        if (!hal::ioPresent()) return false;
        const bool pressed = (hal::ioSwitches() >> (c.src - SRC_SW0)) & 1;
        return c.op == SW_RELEASED ? !pressed : pressed;
    }
    return false;
}

bool ruleActive(int i)
{
    return gRules[constrain(i, 0, MAX_RULES - 1)].enabled && gRt[constrain(i, 0, MAX_RULES - 1)].active;
}

bool ruleControlled(int out)
{
    for (const Rule &r : gRules)
        if (r.enabled && r.c1.src != SRC_NONE && r.out == out) return true;
    return false;
}

bool overridden(int out)
{
    return gOver[out & 3];
}

bool manualToggle(int out)
{
    out &= 3;
    hal::ioToggleLed(out);
    const bool on = (hal::ioLeds() >> out) & 1;
    if (gAuto && ruleControlled(out)) gOver[out] = true;
    logEvent(out, on, "สั่งมือ");
    return on;
}

uint32_t eventSeq()
{
    return gEventSeq;
}

const char *eventText()
{
    return gEvent;
}

}  // namespace farm
