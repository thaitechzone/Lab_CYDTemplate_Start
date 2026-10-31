#include <cmath>

#include "app_config.h"
#include "board_config.h"
#include "hal.h"
#include "ui.h"

// =====================================================================
//  แท็บ "ADC": แสดงผล ADS1115 A0-A3 เป็น 2 มุมมอง (สลับด้วยแถบด้านบน)
//    เกจ   เกจเข็ม 4 ช่อง (เข็มวิ่งนุ่มนวล, แตะเลือกช่อง) + การ์ดรายละเอียด/min/max/เสียงเตือน
//    กราฟ  เส้น 4 ช่องย้อนหลัง ~12 วินาที, แตะที่ป้ายช่องเพื่อซ่อน/แสดงเส้น, หยุด/ล้างได้
// =====================================================================

namespace {

constexpr int N = 4;
constexpr lv_palette_t CH_PAL[N] = {LV_PALETTE_CYAN, LV_PALETTE_ORANGE, LV_PALETTE_GREEN, LV_PALETTE_PINK};
constexpr uint16_t CH_NOTE[N] = {523, 659, 784, 1047};  // C5 E5 G5 C6
constexpr int METER_SIZE = 104;
constexpr int SCALE_MAX = static_cast<int>(app::ADC_FULLSCALE_V * 100 + 0.5f);  // เกจนับทีละ 0.01 V
constexpr int POINTS = 150;
constexpr int MV_MAX = static_cast<int>(app::ADC_FULLSCALE_V * 1000 + 0.5f);

lv_obj_t *gDot, *gStatus, *gRate;
lv_obj_t *gSeg, *gViewGauge, *gViewGraph;
lv_obj_t *gMeter[N], *gMeterVal[N];
lv_meter_indicator_t *gNeedle[N];
lv_obj_t *gDetailName, *gDetailVal, *gDetailBar, *gDetailInfo, *gAlarmSw;
lv_obj_t *gChart, *gLegend[N], *gPauseBtn;
lv_chart_series_t *gSer[N];

float gDisp[N];       // แรงดันที่แสดง (V) วิ่งเข้าหาค่าจริงแบบนุ่มนวล
float gMin[N], gMax[N];
int gSelGlow[N];      // 0-100 ไฮไลต์ของเกจที่ถูกเลือก (animate)
int gSel = 0;
int gShownPresent = -1;
bool gSweep = false;  // true = กำลังเล่น animation กวาดเข็มตอนต่อชิปได้
bool gAlarmed = false;
int gSlideDir = 1;

// ---------------------------------------------------------------------
//  ตัวช่วย
// ---------------------------------------------------------------------
void setText(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void resetMinMax()
{
    for (int i = 0; i < N; i++) {
        gMin[i] = 99.0f;
        gMax[i] = -1.0f;
    }
}

int indexOf(lv_obj_t *const *arr, void *var)
{
    for (int i = 0; i < N; i++)
        if (arr[i] == var) return i;
    return 0;
}

void paintSel(int i)
{
    const int g = LV_CLAMP(0, gSelGlow[i], 100);
    const lv_color_t c = lv_palette_main(CH_PAL[i]);
    lv_obj_t *m = gMeter[i];
    lv_obj_set_style_border_width(m, 2, 0);
    lv_obj_set_style_border_color(m, lv_color_mix(c, lv_color_hex(0x3A4252), g * 255 / 100), 0);
    lv_obj_set_style_shadow_color(m, c, 0);
    lv_obj_set_style_shadow_width(m, g * 18 / 100, 0);
    lv_obj_set_style_shadow_spread(m, g * 2 / 100, 0);
    lv_obj_set_style_shadow_opa(m, g * LV_OPA_60 / 100, 0);
}

void selExec(void *var, int32_t v)
{
    const int i = indexOf(gMeter, var);
    gSelGlow[i] = v;
    paintSel(i);
}

void viewExec(void *var, int32_t v)
{
    lv_obj_t *o = static_cast<lv_obj_t *>(var);
    lv_obj_set_style_opa(o, v * 255 / 100, 0);
    lv_obj_set_style_translate_x(o, gSlideDir * (100 - v) * 36 / 100, 0);
}

void sweepExec(void *, int32_t v)  // 0..200: ขึ้นสุดแล้วกลับ
{
    const float frac = (v <= 100 ? v : 200 - v) / 100.0f;
    for (int i = 0; i < N; i++) gDisp[i] = frac * app::ADC_FULLSCALE_V;
}

void sweepDone(lv_anim_t *)
{
    gSweep = false;
}

void dotExec(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(var), v, 0);
}

void anim(void *var, lv_anim_exec_xcb_t cb, int from, int to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_ready_cb_t ready = nullptr)
{
    lv_anim_del(var, cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, path);
    if (ready != nullptr) lv_anim_set_ready_cb(&a, ready);
    lv_anim_start(&a);
}

void showView(int index)
{
    lv_obj_t *show = index == 0 ? gViewGauge : gViewGraph;
    lv_obj_t *hide = index == 0 ? gViewGraph : gViewGauge;
    lv_obj_add_flag(hide, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(show, LV_OBJ_FLAG_HIDDEN);
    gSlideDir = index == 0 ? -1 : 1;
    anim(show, viewExec, 0, 100, 280, lv_anim_path_ease_out);
}

void select(int i, bool sound)
{
    if (i != gSel) anim(gMeter[gSel], selExec, gSelGlow[gSel], 0, 300, lv_anim_path_ease_out);
    gSel = i;
    anim(gMeter[i], selExec, gSelGlow[i], 100, 350, lv_anim_path_ease_out);
    gAlarmed = false;
    if (sound) hal::soundTone(CH_NOTE[i], 60);
}

// ---------------------------------------------------------------------
//  เหตุการณ์จากผู้ใช้
// ---------------------------------------------------------------------
void onSeg(lv_event_t *)
{
    const uint32_t id = lv_btnmatrix_get_selected_btn(gSeg);
    if (id > 1) return;
    showView(id);
    hal::soundTone(id == 0 ? 698 : 880, 50);
}

void onMeterTap(lv_event_t *e)
{
    select(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))), true);
}

void onResetMinMax(lv_event_t *)
{
    resetMinMax();
    hal::soundTone(880, 50);
}

void onAlarmSw(lv_event_t *)
{
    gAlarmed = false;
    hal::soundTone(lv_obj_has_state(gAlarmSw, LV_STATE_CHECKED) ? 988 : 523, 50);
}

void onLegend(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const bool on = lv_obj_has_state(gLegend[i], LV_STATE_CHECKED);
    lv_chart_hide_series(gChart, gSer[i], !on);
    hal::soundTone(on ? 988 : 523, 50);
}

void onPause(lv_event_t *)
{
    hal::soundTone(lv_obj_has_state(gPauseBtn, LV_STATE_CHECKED) ? 440 : 784, 50);
}

void onClear(lv_event_t *)
{
    for (int i = 0; i < N; i++) lv_chart_set_all_value(gChart, gSer[i], LV_CHART_POINT_NONE);
    lv_chart_refresh(gChart);
    hal::soundSweep(900, 300, 120);
}

// ---------------------------------------------------------------------
//  อัปเดตตามข้อมูลจริง
// ---------------------------------------------------------------------
void setPresent(bool present)
{
    char buf[96];
    lv_obj_set_style_bg_color(gDot, lv_palette_main(present ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
    if (present) snprintf(buf, sizeof(buf), "ADS1115 @0x%02X  เชื่อมต่อแล้ว", cyd::ADS1115_ADDR);
    else snprintf(buf, sizeof(buf), "ไม่พบ ADS1115 @0x%02X ตรวจสาย SDA%d SCL%d และขา ADDR", cyd::ADS1115_ADDR, cyd::I2C_PIN_SDA, cyd::I2C_PIN_SCL);
    lv_label_set_text(gStatus, buf);
    lv_obj_set_style_text_color(gStatus, lv_palette_main(present ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
    if (present) {  // เข็มกวาดขึ้นสุดแล้วกลับ เหมือนเกจรถยนต์ตอนสตาร์ท
        gSweep = true;
        anim(gViewGauge, sweepExec, 0, 200, 1100, lv_anim_path_ease_in_out, sweepDone);
    }
    if (gShownPresent != -1) hal::soundPlay(present ? hal::Melody::Success : hal::Melody::Error);
    gShownPresent = present;
}

lv_color_t zoneColor(float frac)
{
    return lv_palette_main(frac < 0.60f ? LV_PALETTE_GREEN : (frac < 0.85f ? LV_PALETTE_AMBER : LV_PALETTE_RED));
}

void tick(lv_timer_t *)
{
    const uint32_t now = millis();
    const bool present = hal::adsPresent();
    if (gShownPresent != present) setPresent(present);

    // ---- ค่าจริง -> ค่าที่แสดง (นุ่มนวล) + min/max ----
    for (int i = 0; i < N; i++) {
        if (present) {
            const float v = hal::adsVolts(i);
            if (v < gMin[i]) gMin[i] = v;
            if (v > gMax[i]) gMax[i] = v;
            if (!gSweep) gDisp[i] += (v - gDisp[i]) * 0.3f;
        } else if (!gSweep) {
            gDisp[i] += (0.0f - gDisp[i]) * 0.2f;
        }
    }

    // ---- เกจ ----
    char buf[80];
    for (int i = 0; i < N; i++) {
        lv_meter_set_indicator_value(gMeter[i], gNeedle[i], static_cast<int32_t>(lroundf(gDisp[i] * 100)));
        snprintf(buf, sizeof(buf), present ? "%.2f V" : "-- V", gDisp[i]);
        setText(gMeterVal[i], buf);
    }

    // ---- ข้อความรายละเอียด (จำกัดความถี่ให้อ่านทัน) ----
    static uint32_t lastText = 0;
    if (now - lastText >= 100) {
        lastText = now;
        const float v = gDisp[gSel], frac = v / app::ADC_FULLSCALE_V;
        snprintf(buf, sizeof(buf), present ? "%.3f V" : "-- V", v);
        setText(gDetailVal, buf);
        lv_obj_set_style_text_color(gDetailVal, present && v >= app::ADC_ALARM_V ? lv_palette_main(LV_PALETTE_RED) : lv_color_white(), 0);
        if (present && gMax[gSel] >= 0)
            snprintf(buf, sizeof(buf), "ดิบ %d  |  %d%%\nต่ำสุด %.3f  สูงสุด %.3f V", hal::adsRaw(gSel), static_cast<int>(lroundf(frac * 100)), gMin[gSel], gMax[gSel]);
        else snprintf(buf, sizeof(buf), "ไม่มีข้อมูล");
        setText(gDetailInfo, buf);
        for (int i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "A%d %.2f", i, gDisp[i]);
            lv_obj_t *l = lv_obj_get_child(gLegend[i], 0);
            setText(l, buf);
        }
    }
    lv_bar_set_value(gDetailBar, LV_CLAMP(0, static_cast<int>(gDisp[gSel] / app::ADC_FULLSCALE_V * 1000), 1000), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(gDetailBar, zoneColor(gDisp[gSel] / app::ADC_FULLSCALE_V), LV_PART_INDICATOR);

    // ---- เสียงเตือน ----
    const float vs = gDisp[gSel];
    if (lv_obj_has_state(gAlarmSw, LV_STATE_CHECKED) && present) {
        if (vs >= app::ADC_ALARM_V && !gAlarmed) {
            gAlarmed = true;
            hal::soundTone(1760, 140);
        } else if (vs < app::ADC_ALARM_V - 0.1f) {
            gAlarmed = false;
        }
    }

    // ---- กราฟ: เพิ่มจุดเป็นจังหวะคงที่ ----
    static uint32_t lastPoint = 0;
    if (now - lastPoint >= app::ADC_GRAPH_MS) {
        lastPoint = now;
        if (!lv_obj_has_state(gPauseBtn, LV_STATE_CHECKED)) {
            for (int i = 0; i < N; i++) lv_chart_set_next_value(gChart, gSer[i], present ? static_cast<lv_coord_t>(lroundf(gDisp[i] * 1000)) : LV_CHART_POINT_NONE);
        }
    }

    // ---- อัตราสุ่ม ----
    static uint32_t lastRate = 0, lastSweeps = 0;
    if (now - lastRate >= 1000) {
        const uint32_t sw = hal::adsSweeps();
        snprintf(buf, sizeof(buf), present ? "สุ่ม %u รอบ/วินาที (ครบ 4 ช่อง)" : "", static_cast<unsigned>((sw - lastSweeps) * 1000 / (now - lastRate)));
        setText(gRate, buf);
        lastRate = now;
        lastSweeps = sw;
    }
}

// ---------------------------------------------------------------------
//  สร้างหน้าจอ
// ---------------------------------------------------------------------
lv_obj_t *plainBox(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

void buildGauge(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, "เกจแรงดัน (แตะเพื่อเลือกช่อง)");
    lv_obj_t *grid = ui_row(card);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < N; i++) {
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 2, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *m = lv_meter_create(cell);
        gMeter[i] = m;
        lv_obj_set_size(m, METER_SIZE, METER_SIZE);
        lv_obj_set_style_bg_color(m, lv_color_hex(0x121824), 0);
        lv_obj_set_style_pad_all(m, 6, 0);
        lv_obj_set_style_text_opa(m, LV_OPA_TRANSP, LV_PART_TICKS);  // ซ่อนตัวเลขบนสเกล เหลือขีดอย่างเดียว
        lv_obj_set_style_bg_color(m, lv_palette_main(CH_PAL[i]), LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(m, 5, LV_PART_INDICATOR);  // จุดหมุนของเข็ม
        lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(m, onMeterTap, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_meter_scale_t *s = lv_meter_add_scale(m);
        lv_meter_set_scale_ticks(m, s, SCALE_MAX / 10 + 1, 1, 4, lv_palette_darken(LV_PALETTE_GREY, 1));  // ขีดทุก 0.1 V
        lv_meter_set_scale_major_ticks(m, s, 10, 2, 8, lv_color_white(), 6);                            // ขีดใหญ่ทุก 1 V
        lv_meter_set_scale_range(m, s, 0, SCALE_MAX, 240, 150);

        // โซนสี เขียว / เหลือง / แดง
        const int z1 = SCALE_MAX * 60 / 100, z2 = SCALE_MAX * 85 / 100;
        lv_meter_indicator_t *a = lv_meter_add_arc(m, s, 4, lv_palette_main(LV_PALETTE_GREEN), -12);
        lv_meter_set_indicator_start_value(m, a, 0);
        lv_meter_set_indicator_end_value(m, a, z1);
        a = lv_meter_add_arc(m, s, 4, lv_palette_main(LV_PALETTE_AMBER), -12);
        lv_meter_set_indicator_start_value(m, a, z1);
        lv_meter_set_indicator_end_value(m, a, z2);
        a = lv_meter_add_arc(m, s, 4, lv_palette_main(LV_PALETTE_RED), -12);
        lv_meter_set_indicator_start_value(m, a, z2);
        lv_meter_set_indicator_end_value(m, a, SCALE_MAX);

        gNeedle[i] = lv_meter_add_needle_line(m, s, 3, lv_palette_lighten(CH_PAL[i], 2), -10);
        lv_meter_set_indicator_value(m, gNeedle[i], 0);

        gMeterVal[i] = ui_label(m, "-- V");
        lv_obj_align(gMeterVal[i], LV_ALIGN_BOTTOM_MID, 0, -2);

        char buf[24];
        snprintf(buf, sizeof(buf), "A%d  AIN%d", i, i);
        ui_label(cell, buf, nullptr, lv_palette_main(CH_PAL[i]));
        paintSel(i);
    }

    card = ui_card(parent, nullptr);
    lv_obj_t *row = ui_row(card);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    gDetailName = ui_label(row, "ช่องที่เลือก", nullptr, lv_palette_lighten(LV_PALETTE_GREY, 2));
    gDetailVal = ui_label(row, "-- V", &font_th_24);
    gDetailBar = lv_bar_create(card);
    lv_obj_set_size(gDetailBar, LV_PCT(100), 10);
    lv_bar_set_range(gDetailBar, 0, 1000);
    gDetailInfo = ui_label(card, "", nullptr, lv_palette_lighten(LV_PALETTE_GREY, 2));
    lv_obj_set_width(gDetailInfo, LV_PCT(100));
    row = ui_row(card);
    ui_btn(row, LV_SYMBOL_REFRESH " รีเซ็ต min/max", onResetMinMax);
    gAlarmSw = ui_switch_row(card, "เตือนเสียงเมื่อเกิน 3.0 V", false, onAlarmSw);
}

void buildGraph(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent, "กราฟ A0-A3 (ย้อนหลัง ~12 วินาที)");
    gChart = lv_chart_create(card);
    lv_obj_set_size(gChart, LV_PCT(100), 118);
    lv_chart_set_type(gChart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(gChart, POINTS);
    lv_chart_set_range(gChart, LV_CHART_AXIS_PRIMARY_Y, 0, MV_MAX);
    lv_chart_set_div_line_count(gChart, 5, 7);
    lv_chart_set_update_mode(gChart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_color(gChart, lv_color_hex(0x0E131C), 0);
    lv_obj_set_style_border_color(gChart, lv_color_hex(0x3A4252), 0);
    lv_obj_set_style_line_color(gChart, lv_color_hex(0x222B3A), LV_PART_MAIN);
    lv_obj_set_style_size(gChart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(gChart, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(gChart, 4, 0);
    ui_no_scroll(gChart);
    for (int i = 0; i < N; i++) {
        gSer[i] = lv_chart_add_series(gChart, lv_palette_main(CH_PAL[i]), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_value(gChart, gSer[i], LV_CHART_POINT_NONE);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fV", app::ADC_FULLSCALE_V);
    lv_obj_t *l = ui_label(gChart, buf, nullptr, lv_palette_darken(LV_PALETTE_GREY, 0));
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, 0);
    l = ui_label(gChart, "0V", nullptr, lv_palette_darken(LV_PALETTE_GREY, 0));
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 2, 0);

    card = ui_card(parent, "แตะป้ายเพื่อซ่อน/แสดงเส้น");
    lv_obj_t *row = ui_row(card);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < N; i++) {
        lv_obj_t *b = lv_btn_create(row);
        gLegend[i] = b;
        lv_obj_set_size(b, 64, 32);
        lv_obj_set_style_pad_all(b, 2, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_state(b, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x252C3A), 0);
        lv_obj_set_style_bg_color(b, lv_palette_darken(CH_PAL[i], 2), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(b, lv_palette_main(CH_PAL[i]), LV_STATE_CHECKED);
        lv_obj_set_style_border_width(b, 2, LV_STATE_CHECKED);
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text_fmt(t, "A%d --", i);
        lv_obj_center(t);
        lv_obj_add_event_cb(b, onLegend, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    row = ui_row(card);
    gPauseBtn = ui_btn(row, LV_SYMBOL_PAUSE " หยุดกราฟ", onPause, nullptr, LV_EVENT_VALUE_CHANGED);
    lv_obj_add_flag(gPauseBtn, LV_OBJ_FLAG_CHECKABLE);
    ui_btn(row, LV_SYMBOL_TRASH " ล้าง", onClear);
}

}  // namespace

void ui_adc_tab(lv_obj_t *tab)
{
    resetMinMax();

    // ---- สถานะการเชื่อมต่อ ----
    lv_obj_t *card = ui_card(tab, nullptr);
    lv_obj_t *row = ui_row(card);
    gDot = lv_obj_create(row);
    lv_obj_remove_style_all(gDot);
    lv_obj_set_size(gDot, 12, 12);
    lv_obj_set_style_radius(gDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(gDot, LV_OPA_COVER, 0);
    gStatus = ui_label(row, "");
    lv_obj_set_width(gStatus, 250);
    gRate = ui_label(card, "", nullptr, lv_palette_main(LV_PALETTE_CYAN));
    {  // จุดสถานะเต้นเป็นจังหวะต่อเนื่อง
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

    // ---- แถบสลับมุมมอง ----
    static const char *segMap[] = {"เกจ", "กราฟ", ""};
    gSeg = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(gSeg, segMap);
    lv_btnmatrix_set_btn_ctrl_all(gSeg, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(gSeg, true);
    lv_btnmatrix_set_btn_ctrl(gSeg, 0, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_size(gSeg, LV_PCT(100), 38);
    lv_obj_set_style_pad_all(gSeg, 3, 0);
    lv_obj_add_event_cb(gSeg, onSeg, LV_EVENT_VALUE_CHANGED, nullptr);
    ui_no_scroll(gSeg);

    gViewGauge = plainBox(tab);
    gViewGraph = plainBox(tab);
    buildGauge(gViewGauge);
    buildGraph(gViewGraph);
    lv_obj_add_flag(gViewGraph, LV_OBJ_FLAG_HIDDEN);

    select(0, false);
    lv_timer_create(tick, 33, nullptr);
}
