#include "app_config.h"
#include "board_config.h"
#include "hal.h"
#include "ui.h"

// =====================================================================
//  แท็บ "I/O": ควบคุมและแสดงผล PCF8574
//    LED0-LED3 (P0-P3, Isink)         แตะเพื่อสลับ / ปุ่มรูปแบบ (วิ่ง กระพริบ นับไบนารี)
//    SW0-SW3   (P4-P7, Active Low)    กดสวิตช์จริงแล้วปุ่มบนจอตอบสนอง (ripple + เสียง) และสลับ LED ตามได้
//  ทุกอย่างบนจอ "ตามสถานะจริงของชิป" ใน tick() จึงเคลื่อนไหวเองไม่ว่าสั่งจากจอหรือสวิตช์จริง
// =====================================================================

namespace {

constexpr int N = 4;
enum class Mode : uint8_t { Manual, Chase, Blink, Count };

constexpr lv_palette_t LED_PAL[N] = {LV_PALETTE_RED, LV_PALETTE_ORANGE, LV_PALETTE_GREEN, LV_PALETTE_LIGHT_BLUE};
constexpr uint16_t SW_NOTE[N] = {523, 659, 784, 1047};  // C5 E5 G5 C6
constexpr int CHASE_SEQ[] = {0, 1, 2, 3, 2, 1};         // วิ่งไปกลับ
constexpr int LED_SIZE = 52;
constexpr int SW_W = 64, SW_H = 54;

lv_obj_t *gDot, *gStatus, *gRawLabel;
lv_obj_t *gLed[N], *gSw[N], *gSwState[N], *gSwCap[N];
lv_obj_t *gModeBtn[3], *gSpeed, *gSpeedLabel, *gLinkSw;
int gGlow[N];           // 0-100 ความสว่างบนจอของ LED (animate เข้าหาสถานะจริง)
int gPress[N];          // 0-100 ระดับการกดของปุ่มสวิตช์ (เกินช่วงได้ชั่วคราวจาก overshoot)
bool gLedShown[N];      // สถานะที่จอแสดง/animate ถึงแล้ว
int gPressCount[N];
uint8_t gPrevSw = 0;
int gShownRaw = -1;
int gShownPresent = -1;  // -1 = ยังไม่เคยแสดง (ครั้งแรกไม่ส่งเสียง)
Mode gMode = Mode::Manual;
uint32_t gStepAt = 0;
int gStep = 0;

int indexOf(lv_obj_t *const *arr, void *var)
{
    for (int i = 0; i < N; i++)
        if (arr[i] == var) return i;
    return 0;
}

// ---------------------------------------------------------------------
//  วาด LED / ปุ่มสวิตช์ จากค่า animate
// ---------------------------------------------------------------------
void paintLed(int i)
{
    const int g = LV_CLAMP(0, gGlow[i], 100);
    const lv_color_t c = lv_palette_main(LED_PAL[i]);
    lv_obj_t *o = gLed[i];
    lv_obj_set_style_bg_color(o, lv_color_mix(c, lv_color_hex(0x1A202C), g * 255 / 100), 0);
    lv_obj_set_style_border_color(o, lv_color_mix(lv_color_lighten(c, LV_OPA_40), lv_color_hex(0x3A4252), g * 255 / 100), 0);
    lv_obj_set_style_shadow_color(o, c, 0);
    lv_obj_set_style_shadow_width(o, g * 26 / 100, 0);
    lv_obj_set_style_shadow_spread(o, g * 5 / 100, 0);
    lv_obj_set_style_shadow_opa(o, g * LV_OPA_70 / 100, 0);
}

void paintSw(int i)
{
    const int p = LV_CLAMP(0, gPress[i], 100);
    const lv_color_t c = lv_palette_main(LED_PAL[i]);
    lv_obj_t *o = gSw[i];
    lv_obj_set_style_bg_color(o, lv_color_mix(c, lv_color_hex(0x232A38), p * 255 / 100), 0);
    lv_obj_set_style_border_color(o, lv_color_mix(lv_color_lighten(c, LV_OPA_30), lv_color_hex(0x4A5468), p * 255 / 100), 0);
    lv_obj_set_style_transform_zoom(o, 256 - gPress[i] * 32 / 100, 0);  // กดแล้วยุบลง ปล่อยแล้วเด้ง (overshoot)
}

void ledExec(void *var, int32_t v)
{
    const int i = indexOf(gLed, var);
    gGlow[i] = v;
    paintLed(i);
}

void swExec(void *var, int32_t v)
{
    const int i = indexOf(gSw, var);
    gPress[i] = v;
    paintSw(i);
}

void rippleExec(void *var, int32_t v)
{
    lv_obj_t *r = static_cast<lv_obj_t *>(var);
    const int s = 10 + v * 80 / 100;
    lv_obj_set_size(r, s, s);
    lv_obj_center(r);
    lv_obj_set_style_bg_opa(r, (100 - v) * 150 / 100, 0);
}

void dotExec(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(var), v, 0);
}

void animate(lv_obj_t *var, lv_anim_exec_xcb_t cb, int from, int to, uint32_t ms, lv_anim_path_cb_t path)
{
    lv_anim_del(var, cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

// คลื่นวงกลมขยายจากกลาง widget แล้วจางหาย (ถูกตัดตามขอบโค้งของ parent)
void ripple(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_add_flag(r, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(r, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(r, 150, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, r);
    lv_anim_set_exec_cb(&a, rippleExec);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 420);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, lv_obj_del_anim_ready_cb);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------------
//  การทำงาน
// ---------------------------------------------------------------------
void setMode(Mode m)
{
    gMode = m;
    gStep = 0;
    gStepAt = 0;
    for (int k = 0; k < 3; k++) {
        if (static_cast<int>(m) == k + 1) lv_obj_add_state(gModeBtn[k], LV_STATE_CHECKED);
        else lv_obj_clear_state(gModeBtn[k], LV_STATE_CHECKED);
    }
}

void onLedTap(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (!hal::ioPresent()) {
        hal::soundPlay(hal::Melody::Error);
        return;
    }
    setMode(Mode::Manual);
    hal::ioToggleLed(i);
    ripple(gLed[i]);
    hal::soundTone((hal::ioLeds() >> i) & 1 ? 1319 : 659, 70);  // ติด = สูง, ดับ = ต่ำ
}

void onAll(lv_event_t *e)
{
    const bool on = lv_event_get_user_data(e) != nullptr;
    setMode(Mode::Manual);
    hal::ioSetLeds(on ? 0x0F : 0x00);
    hal::soundSweep(on ? 600 : 1500, on ? 1500 : 600, 160);
}

void onMode(lv_event_t *e)
{
    const Mode m = static_cast<Mode>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const bool off = gMode == m;  // กดซ้ำ = หยุด
    setMode(off ? Mode::Manual : m);
    if (!off) hal::ioSetLeds(0);
    hal::soundTone(off ? 440 : 880, 60);
}

void onSpeed(lv_event_t *)
{
    lv_label_set_text_fmt(gSpeedLabel, "ความเร็ว %d ms/ขั้น", static_cast<int>(lv_slider_get_value(gSpeed)));
}

void onRecal(lv_event_t *)
{
    ui_calibrate_start(false);
}

void onLink(lv_event_t *)
{
    hal::soundTone(lv_obj_has_state(gLinkSw, LV_STATE_CHECKED) ? 988 : 523, 50);
}

void setPresent(bool present)
{
    char buf[96];
    lv_obj_set_style_bg_color(gDot, lv_palette_main(present ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
    if (present) snprintf(buf, sizeof(buf), "PCF8574 @0x%02X  เชื่อมต่อแล้ว", cyd::PCF8574_ADDR);
    else snprintf(buf, sizeof(buf), "ไม่พบ PCF8574 @0x%02X ตรวจสาย SDA%d SCL%d", cyd::PCF8574_ADDR, cyd::I2C_PIN_SDA, cyd::I2C_PIN_SCL);
    lv_label_set_text(gStatus, buf);
    lv_obj_set_style_text_color(gStatus, lv_palette_main(present ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
    if (gShownPresent != -1) hal::soundPlay(present ? hal::Melody::Success : hal::Melody::Error);
    gShownPresent = present;
}

void showRaw(uint8_t raw)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "P7-P0  %c%c%c%c %c%c%c%c   0x%02X", (raw & 0x80) ? '1' : '0', (raw & 0x40) ? '1' : '0', (raw & 0x20) ? '1' : '0',
             (raw & 0x10) ? '1' : '0', (raw & 0x08) ? '1' : '0', (raw & 0x04) ? '1' : '0', (raw & 0x02) ? '1' : '0', (raw & 0x01) ? '1' : '0', raw);
    lv_label_set_text(gRawLabel, buf);
    gShownRaw = raw;
}

void onSwitchEdge(int i, bool down)
{
    animate(gSw[i], swExec, gPress[i], down ? 100 : 0, down ? 110 : 420, down ? lv_anim_path_ease_out : lv_anim_path_overshoot);
    lv_label_set_text(gSwState[i], down ? "กด" : "ปล่อย");
    lv_obj_set_style_text_color(gSwState[i], down ? lv_color_white() : lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    if (!down) return;
    ripple(gSw[i]);
    gPressCount[i]++;
    lv_label_set_text_fmt(gSwCap[i], "P%d  x%d", i + 4, gPressCount[i]);
    hal::soundTone(SW_NOTE[i], 70);
    Serial.printf("[PCF] SW%d pressed (P%d) count=%d\n", i, i + 4, gPressCount[i]);
    if (lv_obj_has_state(gLinkSw, LV_STATE_CHECKED)) {
        setMode(Mode::Manual);
        hal::ioToggleLed(i);
    }
}

void stepPattern(uint32_t now)
{
    if (gMode == Mode::Manual) return;
    const uint32_t interval = lv_slider_get_value(gSpeed);
    if (gStepAt != 0 && now - gStepAt < interval) return;
    gStepAt = now;
    switch (gMode) {
        case Mode::Chase: hal::ioSetLeds(1u << CHASE_SEQ[gStep]); gStep = (gStep + 1) % (sizeof(CHASE_SEQ) / sizeof(CHASE_SEQ[0])); break;
        case Mode::Blink: hal::ioSetLeds((gStep & 1) ? 0x00 : 0x0F); gStep++; break;
        case Mode::Count: hal::ioSetLeds(gStep & 0x0F); gStep = (gStep + 1) & 0x0F; break;
        default: break;
    }
}

void tick(lv_timer_t *)
{
    const uint32_t now = millis();
    hal::ioPoll(now);

    const bool present = hal::ioPresent();
    if (gShownPresent != present) setPresent(present);
    if (gShownRaw != hal::ioRaw()) showRaw(hal::ioRaw());

    const uint8_t sw = hal::ioSwitches();
    for (int i = 0; i < N; i++) {
        const bool now_ = (sw >> i) & 1, was = (gPrevSw >> i) & 1;
        if (now_ != was) onSwitchEdge(i, now_);
    }
    gPrevSw = sw;

    if (present) stepPattern(now);

    for (int i = 0; i < N; i++) {  // จอตามสถานะจริงของชิป (ชิปหาย = ดับ)
        const bool want = present && ((hal::ioLeds() >> i) & 1);
        if (want != gLedShown[i]) {
            gLedShown[i] = want;
            animate(gLed[i], ledExec, gGlow[i], want ? 100 : 0, want ? 220 : 380, lv_anim_path_ease_out);
        }
    }
}

// ---------------------------------------------------------------------
//  สร้างหน้าจอ
// ---------------------------------------------------------------------
lv_obj_t *evenRow(lv_obj_t *parent)
{
    lv_obj_t *r = ui_row(parent);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    return r;
}

lv_obj_t *cell(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return c;
}

lv_obj_t *roundBox(lv_obj_t *parent, int w, int h, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, h / 2, 0);
    return o;
}

}  // namespace

void ui_io_tab(lv_obj_t *tab)
{
    // ---- สถานะการเชื่อมต่อ ----
    lv_obj_t *card = ui_card(tab, nullptr);
    lv_obj_t *row = ui_row(card);
    gDot = roundBox(row, 12, 12, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(gDot, 0, 0);
    gStatus = ui_label(row, "");
    lv_obj_set_width(gStatus, 250);
    gRawLabel = ui_label(card, "", nullptr, lv_palette_main(LV_PALETTE_CYAN));
    {  // จุดสถานะกะพริบเบาๆ (pulse) ให้รู้ว่าระบบยังทำงาน
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, gDot);
        lv_anim_set_exec_cb(&a, dotExec);
        lv_anim_set_values(&a, 90, 255);
        lv_anim_set_time(&a, 900);
        lv_anim_set_playback_time(&a, 900);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    // ---- LED ----
    card = ui_card(tab, "LED0-3  (P0-P3, Isink)");
    row = evenRow(card);
    for (int i = 0; i < N; i++) {
        lv_obj_t *c = cell(row);
        gLed[i] = roundBox(c, LED_SIZE, LED_SIZE, LV_RADIUS_CIRCLE);
        lv_obj_add_flag(gLed[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(gLed[i], onLedTap, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        paintLed(i);
        char buf[16];
        snprintf(buf, sizeof(buf), "LED%d  P%d", i, i);
        ui_label(c, buf, nullptr, lv_palette_lighten(LV_PALETTE_GREY, 2));
    }
    row = ui_row(card);
    ui_btn(row, LV_SYMBOL_OK " ติดทั้งหมด", onAll, reinterpret_cast<void *>(1));
    ui_btn(row, LV_SYMBOL_CLOSE " ดับ", onAll, nullptr);
    row = ui_row(card);
    const char *modeName[3] = {"วิ่ง", "กระพริบ", "นับไบนารี"};
    for (int k = 0; k < 3; k++) gModeBtn[k] = ui_btn(row, modeName[k], onMode, reinterpret_cast<void *>(static_cast<intptr_t>(k + 1)));
    gSpeedLabel = ui_label(card, "");
    gSpeed = lv_slider_create(card);
    lv_obj_set_width(gSpeed, LV_PCT(92));
    lv_slider_set_range(gSpeed, 60, 800);
    lv_slider_set_value(gSpeed, 250, LV_ANIM_OFF);
    lv_obj_add_event_cb(gSpeed, onSpeed, LV_EVENT_VALUE_CHANGED, nullptr);
    ui_no_scroll(gSpeed);
    onSpeed(nullptr);

    // ---- สวิตช์ ----
    card = ui_card(tab, "SW0-3  (P4-P7, Active Low)");
    row = evenRow(card);
    for (int i = 0; i < N; i++) {
        lv_obj_t *c = cell(row);
        gSw[i] = roundBox(c, SW_W, SW_H, 12);
        lv_obj_clear_flag(gSw[i], LV_OBJ_FLAG_CLICKABLE);
        char buf[16];
        snprintf(buf, sizeof(buf), "SW%d", i);
        lv_obj_t *name = ui_label(gSw[i], buf, &font_th_24);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 2);
        gSwState[i] = ui_label(gSw[i], "ปล่อย", nullptr, lv_palette_lighten(LV_PALETTE_GREY, 1));
        lv_obj_align(gSwState[i], LV_ALIGN_BOTTOM_MID, 0, -2);
        snprintf(buf, sizeof(buf), "P%d  x0", i + 4);
        gSwCap[i] = ui_label(c, buf, nullptr, lv_palette_lighten(LV_PALETTE_GREY, 2));
        paintSw(i);
    }
    gLinkSw = ui_switch_row(card, "กดสวิตช์ = สลับ LED", true, onLink);

    // ---- จอสัมผัส ----
    card = ui_card(tab, "จอสัมผัส");
    row = ui_row(card);
    ui_btn(row, LV_SYMBOL_SETTINGS " Calibrate (กดค้าง)", onRecal, nullptr, LV_EVENT_LONG_PRESSED);
    lv_obj_t *hint = ui_label(card, "แตะจอมีเสียงติ๊ก  แตะ LED = สลับ + เสียง\nถ้าแตะไม่ตรง กดปุ่ม Calibrate ค้างไว้", nullptr,
                              lv_palette_lighten(LV_PALETTE_GREY, 1));
    lv_obj_set_width(hint, LV_PCT(100));

    setMode(Mode::Manual);
    lv_timer_create(tick, 20, nullptr);
}
