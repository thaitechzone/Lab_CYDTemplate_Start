#include <cmath>

#include "app_config.h"
#include "board_config.h"
#include "farm.h"
#include "hal.h"
#include "ui.h"

// =====================================================================
//  แท็บ "ฟาร์ม": แดชบอร์ดสวนอัจฉริยะ
//    หัว       ชื่อระบบ + สวิตช์ AUTO/มือ + สถานะ I/O, ADC + เหตุการณ์ล่าสุด
//    เซนเซอร์  4 ช่อง (ADS1115) เป็นวงแหวน % วิ่งนุ่มนวล  แตะ = สลับ % / ค่าดิบ
//    อุปกรณ์   4 ตัว (LED0-3) ปั๊มน้ำ/พัดลม หมุนเมื่อเปิด, ไฟปลูกเรืองแสง, เตือนภัยเต้นเป็นจังหวะ  แตะ = สั่งมือ
//  ทุกอย่างตามสถานะจริงใน tick() ไม่ว่าถูกสั่งจากกฎ จอ หรือสวิตช์จริง
// =====================================================================

namespace {

constexpr int N = 4;
constexpr lv_palette_t SENS_PAL[N] = {LV_PALETTE_LIGHT_BLUE, LV_PALETTE_AMBER, LV_PALETTE_ORANGE, LV_PALETTE_CYAN};
constexpr lv_palette_t OUT_PAL[N] = {LV_PALETTE_LIGHT_BLUE, LV_PALETTE_TEAL, LV_PALETTE_YELLOW, LV_PALETTE_RED};
const char *const OUT_ICON[N] = {LV_SYMBOL_TINT, LV_SYMBOL_REFRESH, LV_SYMBOL_CHARGE, LV_SYMBOL_BELL};
constexpr int ICON = 40, SPIN = 50;

lv_obj_t *gDotIo, *gDotAdc, *gModeSw, *gModeLbl, *gEvent, *gSummary, *gBeat;
lv_obj_t *gSTile[N], *gSArc[N], *gSPct[N], *gSInfo[N];
lv_obj_t *gATile[N], *gIcon[N], *gIconLbl[N], *gSpin[N], *gBadge[N], *gBadgeLbl[N], *gState[N];
float gShow[N];
bool gShowRaw[N];
int gGlow[N];
bool gOnShown[N];
uint32_t gEventSeen = 0;
int gShownIo = -1, gShownAdc = -1;
char gBadgeChar[N] = {0, 0, 0, 0};

int indexOf(lv_obj_t *const *arr, void *var)
{
    for (int i = 0; i < N; i++)
        if (arr[i] == var) return i;
    return 0;
}

void setText(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

// ---------------------------------------------------------------------
//  animation
// ---------------------------------------------------------------------
void paintIcon(int i)
{
    const int g = LV_CLAMP(0, gGlow[i], 100);
    const lv_color_t c = lv_palette_main(OUT_PAL[i]);
    lv_obj_t *o = gIcon[i];
    lv_obj_set_style_bg_color(o, lv_color_mix(lv_palette_darken(OUT_PAL[i], 2), lv_color_hex(0x14301F), g * 255 / 100), 0);
    lv_obj_set_style_border_color(o, lv_color_mix(c, lv_color_hex(0x2A5A40), g * 255 / 100), 0);
    lv_obj_set_style_shadow_color(o, c, 0);
    lv_obj_set_style_shadow_width(o, g * 22 / 100, 0);
    lv_obj_set_style_shadow_spread(o, g * 3 / 100, 0);
    lv_obj_set_style_shadow_opa(o, g * LV_OPA_70 / 100, 0);
    lv_obj_set_style_text_color(gIconLbl[i], lv_color_mix(lv_color_white(), lv_color_hex(0x5E7F6D), g * 255 / 100), 0);
}

void glowExec(void *var, int32_t v)
{
    const int i = indexOf(gIcon, var);
    gGlow[i] = v;
    paintIcon(i);
}

void spinExec(void *var, int32_t v)
{
    lv_arc_set_rotation(static_cast<lv_obj_t *>(var), v);
}

void pulseExec(void *var, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), v, 0);
}

void fadeExec(void *var, int32_t v)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), v, 0);
}

void shakeExec(void *var, int32_t v)
{
    lv_obj_set_style_translate_x(static_cast<lv_obj_t *>(var), static_cast<int>(sinf(v * 0.42f) * 6.0f * (100 - v) / 100.0f), 0);
}

void anim(void *var, lv_anim_exec_xcb_t cb, int from, int to, uint32_t ms, lv_anim_path_cb_t path)
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

void loopAnim(void *var, lv_anim_exec_xcb_t cb, int from, int to, uint32_t ms, bool playback)
{
    lv_anim_del(var, cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    if (playback) lv_anim_set_playback_time(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, playback ? lv_anim_path_ease_in_out : lv_anim_path_linear);
    lv_anim_start(&a);
}

// อุปกรณ์เปิด/ปิด: เรืองแสง + ภาพเคลื่อนไหวของแต่ละชนิด (ปั๊ม/พัดลมหมุน, เตือนภัยเต้น)
void setRunning(int i, bool on)
{
    anim(gIcon[i], glowExec, gGlow[i], on ? 100 : 0, on ? 260 : 400, lv_anim_path_ease_out);
    if (i == 0 || i == 1) {
        if (on) {
            lv_obj_clear_flag(gSpin[i], LV_OBJ_FLAG_HIDDEN);
            loopAnim(gSpin[i], spinExec, 0, 360, i == 1 ? 600 : 1100, false);
        } else {
            lv_anim_del(gSpin[i], spinExec);
            lv_obj_add_flag(gSpin[i], LV_OBJ_FLAG_HIDDEN);
        }
    } else if (i == 3) {
        if (on) {
            loopAnim(gATile[i], pulseExec, 130, 255, 380, true);
        } else {
            lv_anim_del(gATile[i], pulseExec);
            lv_obj_set_style_opa(gATile[i], LV_OPA_COVER, 0);
        }
    }
    setText(gState[i], on ? "เปิด" : "ปิด");
    lv_obj_set_style_text_color(gState[i], on ? lv_palette_lighten(OUT_PAL[i], 1) : UI_COL_DIM, 0);
}

// ---------------------------------------------------------------------
//  เหตุการณ์จากผู้ใช้
// ---------------------------------------------------------------------
void onMode(lv_event_t *)
{
    const bool a = lv_obj_has_state(gModeSw, LV_STATE_CHECKED);
    farm::setAutoMode(a);
    hal::soundTone(a ? 1047 : 523, 80);
}

void onActuator(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (!hal::ioPresent()) {
        anim(gATile[i], shakeExec, 0, 100, 380, lv_anim_path_linear);
        hal::soundPlay(hal::Melody::Error);
        return;
    }
    const bool on = farm::manualToggle(i);
    ui_ripple(gATile[i]);
    hal::soundTone(on ? 1319 : 659, 70);
}

void onSensor(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    gShowRaw[i] = !gShowRaw[i];
    ui_ripple(gSTile[i]);
    hal::soundTone(gShowRaw[i] ? 880 : 698, 40);
}

// ---------------------------------------------------------------------
//  อัปเดตตามสถานะจริง
// ---------------------------------------------------------------------
void setDot(lv_obj_t *dot, bool ok)
{
    lv_obj_set_style_bg_color(dot, lv_palette_main(ok ? LV_PALETTE_LIGHT_GREEN : LV_PALETTE_RED), 0);
}

void tick(lv_timer_t *)
{
    const bool io = hal::ioPresent(), ads = hal::adsPresent();
    if (gShownIo != io) {
        gShownIo = io;
        setDot(gDotIo, io);
    }
    if (gShownAdc != ads) {
        gShownAdc = ads;
        setDot(gDotAdc, ads);
    }

    // ---- โหมด ----
    const bool autoM = farm::autoMode();
    if (lv_obj_has_state(gModeSw, LV_STATE_CHECKED) != autoM) {
        if (autoM) lv_obj_add_state(gModeSw, LV_STATE_CHECKED);
        else lv_obj_clear_state(gModeSw, LV_STATE_CHECKED);
    }
    setText(gModeLbl, autoM ? "อัตโนมัติ" : "สั่งมือ");
    lv_obj_set_style_text_color(gModeLbl, autoM ? UI_COL_ACCENT : lv_palette_main(LV_PALETTE_AMBER), 0);

    // ---- เซนเซอร์ ----
    char buf[48];
    for (int i = 0; i < N; i++) {
        const float target = ads ? farm::sensorPct(i) : 0.0f;
        gShow[i] += (target - gShow[i]) * 0.25f;
        const int pct = static_cast<int>(lroundf(gShow[i]));
        if (lv_arc_get_value(gSArc[i]) != pct) lv_arc_set_value(gSArc[i], pct);
        snprintf(buf, sizeof(buf), ads ? "%d" : "--", pct);
        setText(gSPct[i], buf);
        if (!ads) snprintf(buf, sizeof(buf), "ไม่มีสัญญาณ");
        else if (gShowRaw[i]) snprintf(buf, sizeof(buf), "ดิบ %d", hal::adsRaw(i));
        else snprintf(buf, sizeof(buf), "%.2f V", hal::adsVolts(i));
        setText(gSInfo[i], buf);
    }

    // ---- อุปกรณ์ ----
    int ruleOn = 0, ruleEn = 0;
    for (int r = 0; r < farm::MAX_RULES; r++) {
        if (farm::rule(r).enabled && farm::rule(r).c1.src != farm::SRC_NONE) {
            ruleEn++;
            if (farm::ruleActive(r)) ruleOn++;
        }
    }
    for (int i = 0; i < N; i++) {
        const bool on = io && ((hal::ioLeds() >> i) & 1);
        if (on != gOnShown[i]) {
            gOnShown[i] = on;
            setRunning(i, on);
        }
        char ch = 'M';
        if (autoM && farm::ruleControlled(i)) ch = farm::overridden(i) ? '!' : 'A';
        if (ch != gBadgeChar[i]) {
            gBadgeChar[i] = ch;
            const char s[2] = {ch, 0};
            lv_label_set_text(gBadgeLbl[i], s);
            lv_obj_set_style_bg_color(gBadge[i], ch == 'A' ? lv_palette_darken(LV_PALETTE_GREEN, 1) : (ch == '!' ? lv_palette_darken(LV_PALETTE_AMBER, 2) : lv_palette_darken(LV_PALETTE_GREY, 2)), 0);
            if (ch == '!') anim(gBadge[i], pulseExec, 80, 255, 500, lv_anim_path_ease_out);
        }
    }
    snprintf(buf, sizeof(buf), "กฎ: ทำงาน %d / เปิด %d", ruleOn, ruleEn);
    setText(gSummary, buf);

    // ---- เหตุการณ์ล่าสุด (จางเข้าเมื่อมีข้อความใหม่) ----
    if (gEventSeen != farm::eventSeq()) {
        gEventSeen = farm::eventSeq();
        setText(gEvent, farm::eventText());
        anim(gEvent, fadeExec, 40, 255, 450, lv_anim_path_ease_out);
    }
}

// ---------------------------------------------------------------------
//  สร้างหน้าจอ
// ---------------------------------------------------------------------
lv_obj_t *dot(lv_obj_t *parent, int size)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return d;
}

lv_obj_t *tileBox(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_radius(t, 12, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, UI_COL_TILE, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, UI_COL_BORDER, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    return t;
}

void buildHeader(lv_obj_t *tab)
{
    lv_obj_t *card = ui_card(tab, nullptr);
    lv_obj_t *row = ui_row(card);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = ui_row(row);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    gBeat = dot(left, 10);  // จุดหัวใจ: บอกว่าระบบยังทำงาน
    lv_obj_set_style_bg_color(gBeat, UI_COL_ACCENT, 0);
    loopAnim(gBeat, pulseExec, 70, 255, 800, true);
    ui_label(left, "SMART FARM", &font_th_24, UI_COL_ACCENT);

    lv_obj_t *right = ui_row(row);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    gModeLbl = ui_label(right, "อัตโนมัติ", nullptr, UI_COL_ACCENT);
    gModeSw = lv_switch_create(right);
    lv_obj_set_size(gModeSw, 44, 22);
    if (farm::autoMode()) lv_obj_add_state(gModeSw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(gModeSw, onMode, LV_EVENT_VALUE_CHANGED, nullptr);

    row = ui_row(card);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    gDotIo = dot(row, 10);
    ui_label(row, "I/O", nullptr, UI_COL_DIM);
    gDotAdc = dot(row, 10);
    ui_label(row, "ADC", nullptr, UI_COL_DIM);
    gSummary = ui_label(row, "", nullptr, UI_COL_DIM);

    gEvent = ui_label(card, farm::eventText(), nullptr, lv_palette_lighten(LV_PALETTE_LIGHT_GREEN, 2));
    lv_obj_set_width(gEvent, LV_PCT(100));
    gEventSeen = farm::eventSeq();
}

void buildSensors(lv_obj_t *tab)
{
    lv_obj_t *card = ui_card(tab, "เซนเซอร์ในสวน  (แตะ = สลับค่าดิบ)");
    lv_obj_t *grid = ui_row(card);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < N; i++) {
        lv_obj_t *t = tileBox(grid, 142, 58);
        gSTile[i] = t;
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, onSensor, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *arc = lv_arc_create(t);
        gSArc[i] = arc;
        lv_obj_set_size(arc, 46, 46);
        lv_obj_align(arc, LV_ALIGN_LEFT_MID, 4, 0);
        lv_arc_set_rotation(arc, 135);
        lv_arc_set_bg_angles(arc, 0, 270);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 0);
        lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_hex(0x1F3A2B), LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_palette_main(SENS_PAL[i]), LV_PART_INDICATOR);
        gSPct[i] = ui_label(arc, "--");
        lv_obj_center(gSPct[i]);

        lv_obj_t *name = ui_label(t, farm::sensorName(i), nullptr, lv_palette_lighten(SENS_PAL[i], 1));
        lv_obj_set_width(name, 84);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 54, 6);
        gSInfo[i] = ui_label(t, "--", nullptr, UI_COL_DIM);
        lv_obj_set_width(gSInfo[i], 84);
        lv_label_set_long_mode(gSInfo[i], LV_LABEL_LONG_DOT);
        lv_obj_align(gSInfo[i], LV_ALIGN_TOP_LEFT, 54, 30);
    }
}

void buildActuators(lv_obj_t *tab)
{
    lv_obj_t *card = ui_card(tab, "อุปกรณ์  (แตะ = สั่งมือ)");
    lv_obj_t *row = ui_row(card);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (int i = 0; i < N; i++) {
        lv_obj_t *t = tileBox(row, 68, 100);
        gATile[i] = t;
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, onActuator, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *ic = lv_obj_create(t);
        gIcon[i] = ic;
        lv_obj_remove_style_all(ic);
        lv_obj_set_size(ic, ICON, ICON);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ic, 2, 0);
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        gIconLbl[i] = ui_label(ic, OUT_ICON[i], &font_th_24);
        lv_obj_center(gIconLbl[i]);

        lv_obj_t *sp = lv_arc_create(t);  // วงหมุนรอบไอคอน (เฉพาะปั๊ม/พัดลม ขณะเปิด)
        gSpin[i] = sp;
        lv_obj_set_size(sp, SPIN, SPIN);
        lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 3);
        lv_arc_set_bg_angles(sp, 0, 0);
        lv_arc_set_angles(sp, 0, 100);
        lv_obj_remove_style(sp, nullptr, LV_PART_KNOB);
        lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(sp, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, lv_palette_lighten(OUT_PAL[i], 2), LV_PART_INDICATOR);
        lv_obj_add_flag(sp, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *name = ui_label(t, farm::outputName(i), nullptr, lv_color_white());
        lv_obj_set_width(name, 64);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 56);
        gState[i] = ui_label(t, "ปิด", nullptr, UI_COL_DIM);
        lv_obj_set_width(gState[i], 64);
        lv_obj_set_style_text_align(gState[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(gState[i], LV_ALIGN_TOP_MID, 0, 76);

        lv_obj_t *b = dot(t, 18);  // ป้ายโหมด: A = กฎ, M = มือ, ! = สั่งมือแทรกกฎ
        gBadge[i] = b;
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -3, 3);
        gBadgeLbl[i] = lv_label_create(b);
        lv_label_set_text(gBadgeLbl[i], "M");
        lv_obj_center(gBadgeLbl[i]);
        lv_obj_set_style_text_font(gBadgeLbl[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(gBadgeLbl[i], lv_color_white(), 0);

        paintIcon(i);
    }
}

}  // namespace

void ui_farm_tab(lv_obj_t *tab)
{
    buildHeader(tab);
    buildSensors(tab);
    buildActuators(tab);
    lv_timer_create(tick, 33, nullptr);
}
