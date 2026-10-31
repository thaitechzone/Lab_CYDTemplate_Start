#include <cmath>

#include "farm.h"
#include "hal.h"
#include "ui.h"

// =====================================================================
//  แท็บ "กฎ": กำหนดเงื่อนไขอัตโนมัติเอง
//    รายการกฎ 6 ช่อง  สวิตช์เปิด/ปิด, ขอบเรืองแสงเมื่อกฎกำลังทำงาน, แตะเพื่อแก้ไข
//    หน้าแก้กฎ        สไลด์ขึ้นมาเต็มจอ: เงื่อนไข 1 [และ/หรือ เงื่อนไข 2] -> อุปกรณ์ ติด/ดับ + ค้างไว้กี่วินาที
//                     แต่ละเงื่อนไขมี "ตอนนี้" สดๆ บอกว่าจริง/เท็จ จึงลองปรับเกณฑ์แล้วดูผลได้ทันที
// =====================================================================

namespace {

constexpr int R = farm::MAX_RULES;
constexpr char SRC_OPTS[] = "ความชื้นดิน (A0)\nความสว่าง (A1)\nอุณหภูมิ (A2)\nระดับน้ำ (A3)\nSW0 (P4)\nSW1 (P5)\nSW2 (P6)\nSW3 (P7)";
constexpr char ADC_OPS[] = "มากกว่า (>)\nน้อยกว่า (<)";
constexpr char SW_OPS[] = "กดอยู่\nปล่อย\nกดสลับ (toggle)";
constexpr char COMB_OPTS[] = "ไม่ใช้เงื่อนไขที่ 2\nและ (AND)\nหรือ (OR)";
constexpr char OUT_OPTS[] = "ปั๊มน้ำ (LED0)\nพัดลม (LED1)\nไฟปลูก (LED2)\nเตือนภัย (LED3)";
constexpr char ACT_OPTS[] = "ติด เมื่อเงื่อนไขเป็นจริง\nดับ เมื่อเงื่อนไขเป็นจริง";

// ---- รายการกฎ ----
lv_obj_t *gRow[R], *gRowSw[R], *gRowText[R], *gRowDot[R];
int gGlow[R];
bool gGlowOn[R];

// ---- หน้าแก้กฎ ----
struct Editor {
    lv_obj_t *ov = nullptr;
    lv_obj_t *swEn = nullptr;
    lv_obj_t *card2 = nullptr;
    lv_obj_t *ddSrc[2], *ddOp[2], *slVal[2], *lblVal[2], *lblNow[2];
    lv_obj_t *ddComb = nullptr, *ddOut = nullptr, *ddAct = nullptr, *slHold = nullptr, *lblHold = nullptr;
    lv_timer_t *timer = nullptr;
    int idx = 0;
    farm::Rule r;
} gEd;

bool isAdc(uint8_t src)
{
    return src >= farm::SRC_ADC0 && src < farm::SRC_ADC0 + farm::N_ADC;
}

void condText(const farm::Cond &c, char *b, size_t n)
{
    if (isAdc(c.src)) snprintf(b, n, "%s %s %d%%", farm::sensorName(c.src - farm::SRC_ADC0), c.op == farm::ADC_GREATER ? ">" : "<", c.value);
    else if (c.src >= farm::SRC_SW0) snprintf(b, n, "SW%d %s", c.src - farm::SRC_SW0, c.op == farm::SW_PRESSED ? "กดอยู่" : (c.op == farm::SW_RELEASED ? "ปล่อย" : "กดสลับ"));
    else snprintf(b, n, "-");
}

void describe(const farm::Rule &r, char *b, size_t n)
{
    if (r.c1.src == farm::SRC_NONE) {
        snprintf(b, n, "(ว่าง) แตะเพื่อสร้างกฎ");
        return;
    }
    char c1[48], c2[48] = "", hold[24] = "";
    condText(r.c1, c1, sizeof(c1));
    if (r.combine != farm::COMB_NONE) condText(r.c2, c2, sizeof(c2));
    if (r.holdSec > 0) snprintf(hold, sizeof(hold), " ค้าง %d วิ", r.holdSec);
    snprintf(b, n, "ถ้า %s%s%s %s %s %s%s", c1, r.combine == farm::COMB_AND ? " และ " : (r.combine == farm::COMB_OR ? " หรือ " : ""), c2, LV_SYMBOL_RIGHT,
             farm::outputName(r.out), r.onWhenTrue ? "ติด" : "ดับ", hold);
}

// ---------------------------------------------------------------------
//  animation
// ---------------------------------------------------------------------
void paintRow(int i)
{
    const int g = LV_CLAMP(0, gGlow[i], 100);
    lv_obj_t *o = gRow[i];
    lv_obj_set_style_border_color(o, lv_color_mix(UI_COL_ACCENT, UI_COL_BORDER, g * 255 / 100), 0);
    lv_obj_set_style_shadow_color(o, UI_COL_ACCENT, 0);
    lv_obj_set_style_shadow_width(o, g * 14 / 100, 0);
    lv_obj_set_style_shadow_opa(o, g * LV_OPA_50 / 100, 0);
}

void rowGlowExec(void *var, int32_t v)
{
    for (int i = 0; i < R; i++) {
        if (gRow[i] == var) {
            gGlow[i] = v;
            paintRow(i);
            return;
        }
    }
}

void slideExec(void *var, int32_t v)
{
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), (100 - v) * UI_H / 100, 0);
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

// ---------------------------------------------------------------------
//  รายการกฎ
// ---------------------------------------------------------------------
void refreshRow(int i)
{
    const farm::Rule &r = farm::rule(i);
    char buf[160];
    describe(r, buf, sizeof(buf));
    lv_label_set_text(gRowText[i], buf);
    const bool empty = r.c1.src == farm::SRC_NONE;
    lv_obj_set_style_text_color(gRowText[i], empty ? UI_COL_DIM : (r.enabled ? lv_color_white() : lv_palette_darken(LV_PALETTE_GREY, 1)), 0);
    if (r.enabled) lv_obj_add_state(gRowSw[i], LV_STATE_CHECKED);
    else lv_obj_clear_state(gRowSw[i], LV_STATE_CHECKED);
    if (empty) lv_obj_add_state(gRowSw[i], LV_STATE_DISABLED);
    else lv_obj_clear_state(gRowSw[i], LV_STATE_DISABLED);
}

void onRowSwitch(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    farm::rule(i).enabled = lv_obj_has_state(gRowSw[i], LV_STATE_CHECKED);
    farm::saveRules();
    refreshRow(i);
    hal::soundTone(farm::rule(i).enabled ? 988 : 523, 50);
}

void onResetAll(lv_event_t *)
{
    farm::resetRules();
    for (int i = 0; i < R; i++) refreshRow(i);
    hal::soundPlay(hal::Melody::Success);
}

void listTick(lv_timer_t *)
{
    for (int i = 0; i < R; i++) {
        const bool on = farm::ruleActive(i);
        if (on != gGlowOn[i]) {
            gGlowOn[i] = on;
            anim(gRow[i], rowGlowExec, gGlow[i], on ? 100 : 0, on ? 300 : 500, lv_anim_path_ease_out);
            lv_obj_set_style_bg_color(gRowDot[i], on ? UI_COL_ACCENT : lv_palette_darken(LV_PALETTE_GREY, 2), 0);
        }
    }
}

// ---------------------------------------------------------------------
//  หน้าแก้กฎ
// ---------------------------------------------------------------------
void syncCond(int k)
{
    farm::Cond &c = k == 0 ? gEd.r.c1 : gEd.r.c2;
    if (c.src == farm::SRC_NONE) c.src = farm::SRC_ADC0;
    lv_dropdown_set_selected(gEd.ddSrc[k], c.src - 1);
    const bool adc = isAdc(c.src);
    lv_dropdown_set_options(gEd.ddOp[k], adc ? ADC_OPS : SW_OPS);
    lv_dropdown_set_selected(gEd.ddOp[k], c.op);
    if (adc) {
        lv_obj_clear_flag(gEd.slVal[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(gEd.lblVal[k], LV_OBJ_FLAG_HIDDEN);
        lv_slider_set_value(gEd.slVal[k], c.value, LV_ANIM_OFF);
        lv_label_set_text_fmt(gEd.lblVal[k], "เกณฑ์ %d%%  (%.2f V)", c.value, c.value * 0.01f * 3.3f);
    } else {
        lv_obj_add_flag(gEd.slVal[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(gEd.lblVal[k], LV_OBJ_FLAG_HIDDEN);
    }
}

void syncAll()
{
    syncCond(0);
    lv_dropdown_set_selected(gEd.ddComb, gEd.r.combine);
    if (gEd.r.combine != farm::COMB_NONE) {
        syncCond(1);
        lv_obj_clear_flag(gEd.card2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(gEd.card2, LV_OBJ_FLAG_HIDDEN);
    }
    lv_dropdown_set_selected(gEd.ddOut, gEd.r.out);
    lv_dropdown_set_selected(gEd.ddAct, gEd.r.onWhenTrue ? 0 : 1);
    lv_slider_set_value(gEd.slHold, gEd.r.holdSec, LV_ANIM_OFF);
    lv_label_set_text_fmt(gEd.lblHold, gEd.r.holdSec ? "ค้างไว้ต่ออีก %d วินาที" : "ไม่ค้าง (ดับทันที)", gEd.r.holdSec);
    if (gEd.r.enabled) lv_obj_add_state(gEd.swEn, LV_STATE_CHECKED);
    else lv_obj_clear_state(gEd.swEn, LV_STATE_CHECKED);
}

farm::Cond &condOf(int k)
{
    return k == 0 ? gEd.r.c1 : gEd.r.c2;
}

void onSrc(lv_event_t *e)
{
    const int k = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    farm::Cond &c = condOf(k);
    c.src = lv_dropdown_get_selected(gEd.ddSrc[k]) + 1;
    c.op = 0;
    syncCond(k);
    hal::soundTone(740, 40);
}

void onOp(lv_event_t *e)
{
    const int k = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    condOf(k).op = lv_dropdown_get_selected(gEd.ddOp[k]);
    hal::soundTone(740, 40);
}

void onVal(lv_event_t *e)
{
    const int k = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    farm::Cond &c = condOf(k);
    c.value = lv_slider_get_value(gEd.slVal[k]);
    lv_label_set_text_fmt(gEd.lblVal[k], "เกณฑ์ %d%%  (%.2f V)", c.value, c.value * 0.01f * 3.3f);
}

void onComb(lv_event_t *)
{
    gEd.r.combine = lv_dropdown_get_selected(gEd.ddComb);
    if (gEd.r.combine != farm::COMB_NONE) {
        if (gEd.r.c2.src == farm::SRC_NONE) gEd.r.c2 = {farm::SRC_ADC0 + 1, farm::ADC_LESS, 50};
        syncCond(1);
        lv_obj_clear_flag(gEd.card2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(gEd.card2, LV_OBJ_FLAG_HIDDEN);
    }
    hal::soundTone(740, 40);
}

void onOut(lv_event_t *)
{
    gEd.r.out = lv_dropdown_get_selected(gEd.ddOut);
    hal::soundTone(740, 40);
}

void onAct(lv_event_t *)
{
    gEd.r.onWhenTrue = lv_dropdown_get_selected(gEd.ddAct) == 0;
    hal::soundTone(740, 40);
}

void onHold(lv_event_t *)
{
    gEd.r.holdSec = lv_slider_get_value(gEd.slHold);
    lv_label_set_text_fmt(gEd.lblHold, gEd.r.holdSec ? "ค้างไว้ต่ออีก %d วินาที" : "ไม่ค้าง (ดับทันที)", gEd.r.holdSec);
}

// ตัวอย่างสดของแต่ละเงื่อนไข: ค่าตอนนี้ + จริง/เท็จ
void editTick(lv_timer_t *)
{
    for (int k = 0; k < 2; k++) {
        const farm::Cond &c = condOf(k);
        if (k == 1 && gEd.r.combine == farm::COMB_NONE) continue;
        const bool t = farm::condNow(c);
        char buf[64];
        if (isAdc(c.src)) {
            if (hal::adsPresent()) snprintf(buf, sizeof(buf), "ตอนนี้ %d%%  %s", static_cast<int>(lroundf(farm::sensorPct(c.src - farm::SRC_ADC0))), t ? "จริง" : "เท็จ");
            else snprintf(buf, sizeof(buf), "ไม่พบ ADS1115  เท็จ");
        } else {
            snprintf(buf, sizeof(buf), "ตอนนี้ %s  %s", ((hal::ioSwitches() >> (c.src - farm::SRC_SW0)) & 1) ? "กดอยู่" : "ปล่อย", t ? "จริง" : "เท็จ");
        }
        if (strcmp(lv_label_get_text(gEd.lblNow[k]), buf) != 0) lv_label_set_text(gEd.lblNow[k], buf);
        lv_obj_set_style_text_color(gEd.lblNow[k], t ? UI_COL_ACCENT : lv_palette_main(LV_PALETTE_AMBER), 0);
    }
}

void closeEditor()
{
    if (gEd.ov == nullptr) return;
    if (gEd.timer != nullptr) {
        lv_timer_del(gEd.timer);
        gEd.timer = nullptr;
    }
    anim(gEd.ov, slideExec, 100, 0, 220, lv_anim_path_ease_in, lv_obj_del_anim_ready_cb);
    gEd.ov = nullptr;
}

void onSave(lv_event_t *)
{
    gEd.r.enabled = lv_obj_has_state(gEd.swEn, LV_STATE_CHECKED);
    if (gEd.r.combine == farm::COMB_NONE) gEd.r.c2 = farm::Cond();
    farm::rule(gEd.idx) = gEd.r;
    farm::saveRules();
    refreshRow(gEd.idx);
    hal::soundPlay(hal::Melody::Success);
    closeEditor();
}

void onDelete(lv_event_t *)
{
    farm::clearRule(gEd.idx);
    refreshRow(gEd.idx);
    hal::soundTone(440, 120);
    closeEditor();
}

void onCancel(lv_event_t *)
{
    hal::soundTone(523, 50);
    closeEditor();
}

lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, lv_event_cb_t cb, intptr_t user)
{
    lv_obj_t *d = lv_dropdown_create(parent);
    lv_dropdown_set_options(d, opts);
    lv_obj_set_width(d, LV_PCT(100));
    lv_obj_add_event_cb(d, cb, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void *>(user));
    return d;
}

lv_obj_t *buildCond(lv_obj_t *body, int k, const char *title)
{
    lv_obj_t *card = ui_card(body, title);
    gEd.ddSrc[k] = dropdown(card, SRC_OPTS, onSrc, k);
    gEd.ddOp[k] = dropdown(card, ADC_OPS, onOp, k);
    gEd.lblVal[k] = ui_label(card, "");
    gEd.slVal[k] = lv_slider_create(card);
    lv_obj_set_width(gEd.slVal[k], LV_PCT(92));
    lv_slider_set_range(gEd.slVal[k], 0, 100);
    lv_obj_add_event_cb(gEd.slVal[k], onVal, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void *>(static_cast<intptr_t>(k)));
    ui_no_scroll(gEd.slVal[k]);
    gEd.lblNow[k] = ui_label(card, "", nullptr, UI_COL_ACCENT);
    return card;
}

void openEditor(int idx)
{
    if (gEd.ov != nullptr) return;
    gEd.idx = idx;
    gEd.r = farm::rule(idx);
    if (gEd.r.c1.src == farm::SRC_NONE) {  // กฎว่าง: เริ่มจากตัวอย่าง "ความชื้นดิน < 40% -> ปั๊มน้ำ ติด"
        gEd.r = farm::Rule();
        gEd.r.enabled = true;
        gEd.r.c1 = {farm::SRC_ADC0, farm::ADC_LESS, 40};
        gEd.r.out = idx % farm::N_OUT;
    }

    lv_obj_t *ov = ui_overlay();
    gEd.ov = ov;
    lv_obj_set_style_bg_color(ov, UI_COL_BG, 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "แก้ไขกฎ #%d", idx + 1);
    lv_obj_t *head = ui_label(ov, buf, &font_th_24, UI_COL_ACCENT);
    lv_obj_set_pos(head, 10, 4);

    lv_obj_t *body = lv_obj_create(ov);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, UI_W, 162);
    lv_obj_set_pos(body, 0, 36);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, 6, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);

    lv_obj_t *card = ui_card(body, nullptr);
    gEd.swEn = ui_switch_row(card, "เปิดใช้งานกฎนี้", gEd.r.enabled, [](lv_event_t *) { hal::soundTone(660, 40); });

    buildCond(body, 0, "ถ้า (เงื่อนไขที่ 1)");
    card = ui_card(body, "เชื่อมเงื่อนไข");
    gEd.ddComb = dropdown(card, COMB_OPTS, onComb, 0);
    gEd.card2 = buildCond(body, 1, "เงื่อนไขที่ 2");

    card = ui_card(body, "แล้วสั่ง");
    gEd.ddOut = dropdown(card, OUT_OPTS, onOut, 0);
    gEd.ddAct = dropdown(card, ACT_OPTS, onAct, 0);
    gEd.lblHold = ui_label(card, "");
    gEd.slHold = lv_slider_create(card);
    lv_obj_set_width(gEd.slHold, LV_PCT(92));
    lv_slider_set_range(gEd.slHold, 0, 60);
    lv_obj_add_event_cb(gEd.slHold, onHold, LV_EVENT_VALUE_CHANGED, nullptr);
    ui_no_scroll(gEd.slHold);
    ui_label(card, "ปั๊ม/พัดลมควรค้างไว้สักครู่ กันเปิด-ปิดถี่เกินไป", nullptr, UI_COL_DIM);

    // ---- แถบปุ่มล่าง ----
    lv_obj_t *foot = lv_obj_create(ov);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, UI_W, 42);
    lv_obj_set_pos(foot, 0, UI_H - 42);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(foot, UI_COL_CARD, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, UI_COL_BORDER, 0);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *b = ui_btn(foot, LV_SYMBOL_OK " บันทึก", onSave);
    lv_obj_set_style_bg_color(b, lv_palette_darken(LV_PALETTE_GREEN, 1), 0);
    b = ui_btn(foot, LV_SYMBOL_TRASH " ลบกฎ", onDelete);
    lv_obj_set_style_bg_color(b, lv_palette_darken(LV_PALETTE_RED, 2), 0);
    ui_btn(foot, "ยกเลิก", onCancel);

    syncAll();
    gEd.timer = lv_timer_create(editTick, 150, nullptr);
    editTick(nullptr);
    anim(ov, slideExec, 0, 100, 280, lv_anim_path_ease_out);
}

void onRowTap(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    hal::soundTone(784, 40);
    openEditor(i);
}

}  // namespace

void ui_rules_tab(lv_obj_t *tab)
{
    lv_obj_t *hint = ui_label(tab, "แตะกฎเพื่อแก้ไข  ขอบเรืองแสง = กำลังทำงาน", nullptr, UI_COL_DIM);
    lv_obj_set_width(hint, LV_PCT(100));

    for (int i = 0; i < R; i++) {
        lv_obj_t *row = lv_obj_create(tab);
        gRow[i] = row;
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, UI_COL_CARD, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, onRowTap, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        gRowSw[i] = lv_switch_create(row);
        lv_obj_set_size(gRowSw[i], 40, 22);
        lv_obj_add_event_cb(gRowSw[i], onRowSwitch, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        gRowText[i] = lv_label_create(row);
        lv_obj_set_flex_grow(gRowText[i], 1);
        lv_label_set_long_mode(gRowText[i], LV_LABEL_LONG_WRAP);

        gRowDot[i] = lv_obj_create(row);
        lv_obj_remove_style_all(gRowDot[i]);
        lv_obj_set_size(gRowDot[i], 10, 10);
        lv_obj_set_style_radius(gRowDot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(gRowDot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(gRowDot[i], lv_palette_darken(LV_PALETTE_GREY, 2), 0);
        lv_obj_clear_flag(gRowDot[i], LV_OBJ_FLAG_CLICKABLE);

        refreshRow(i);
        paintRow(i);
    }

    lv_obj_t *r = ui_row(tab);
    ui_btn(r, LV_SYMBOL_REFRESH " คืนกฎตัวอย่างสวน", onResetAll);
    lv_timer_create(listTick, 100, nullptr);
}
