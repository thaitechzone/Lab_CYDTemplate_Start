#include <esp_chip_info.h>

#include "app_config.h"
#include "hal.h"
#include "ui.h"

// =====================================================================
//  แท็บ "ระบบ": รวมหน้าตั้งค่า/ทดสอบทั้งหมดไว้ที่เดียว สลับด้วยแถบด้านบน
//    ข้อมูล (บอร์ด/หน่วยความจำ/Serial) | จอ | สัมผัส (calibrate) | RGB | เสียง | I/O (ทดสอบ PCF8574)
// =====================================================================

namespace {

lv_obj_t *gLive;
constexpr int SECTIONS = 6;
lv_obj_t *gSec[SECTIONS];
lv_obj_t *gSeg;
int gCurrent = 0;

void onSeg(lv_event_t *)
{
    const uint32_t id = lv_btnmatrix_get_selected_btn(gSeg);
    if (id >= SECTIONS || static_cast<int>(id) == gCurrent) return;
    ui_view_show(gSec[id], gSec[gCurrent], static_cast<int>(id) > gCurrent ? 1 : -1);
    gCurrent = id;
    hal::soundTone(700 + id * 60, 40);
}

void tick(lv_timer_t *)
{
    const uint32_t s = millis() / 1000;
    lv_label_set_text_fmt(gLive, "เปิดเครื่องมา %lu:%02lu:%02lu\nheap ว่าง %u KB (ต่ำสุด %u KB)\nแสง LDR ดิบ %d (ยิ่งสว่างยิ่งต่ำ)\nไฟหลังจอ %d%%   เสียง %s",
                          static_cast<unsigned long>(s / 3600), static_cast<unsigned long>((s / 60) % 60), static_cast<unsigned long>(s % 60),
                          static_cast<unsigned>(ESP.getFreeHeap() / 1024), static_cast<unsigned>(ESP.getMinFreeHeap() / 1024), hal::ldrRaw(),
                          hal::backlight(), hal::VOLUME_NAMES[hal::soundVolume()]);
}

}  // namespace

static void buildInfo(lv_obj_t *tab)
{
    lv_obj_t *card = ui_card(tab, "บอร์ด");
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %s\nชิป %s rev %d  %d คอร์  %lu MHz\nflash %u MB  โปรแกรม %u KB\nจอ %s 320x240  LVGL %d.%d.%d\nเริ่มทำงานเพราะ: %s",
             app::APP_NAME, app::FW_VERSION, ESP.getChipModel(), chip.revision, chip.cores, static_cast<unsigned long>(ESP.getCpuFreqMHz()),
             static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)), static_cast<unsigned>(ESP.getSketchSize() / 1024), hal::displayDriverName(),
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, hal::resetReasonText());
    ui_label(card, buf);

    card = ui_card(tab, "สถานะ");
    gLive = ui_label(card, "");

    card = ui_card(tab, "คำสั่ง Serial (115200)");
    ui_label(card,
             "h ช่วยเหลือ   i สถานะ\nc calibrate จอสัมผัส   k ค่า calibrate\nd ค่าจอ   r รีเซ็ตค่าจอ\nt พิมพ์ค่าดิบจอสัมผัส (เปิด/ปิด)\nl ทดสอบ LED   s ทดสอบเสียง",
             nullptr, lv_palette_lighten(LV_PALETTE_GREY, 2));
    ui_label(card, "จอเพี้ยนจนใช้ไม่ได้: กดปุ่ม BOOT\nค้าง 1 วินาที = รีเซ็ตค่าจอ", nullptr, lv_palette_main(LV_PALETTE_AMBER));

    lv_timer_create(tick, 1000, nullptr);
    tick(nullptr);
}

void ui_system_tab(lv_obj_t *tab)
{
    static const char *segMap[] = {"ข้อมูล", "จอ", "สัมผัส", "RGB", "เสียง", "I/O", ""};
    gSeg = lv_btnmatrix_create(tab);
    lv_btnmatrix_set_map(gSeg, segMap);
    lv_btnmatrix_set_btn_ctrl_all(gSeg, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(gSeg, true);
    lv_btnmatrix_set_btn_ctrl(gSeg, 0, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_size(gSeg, LV_PCT(100), 38);
    lv_obj_set_style_pad_all(gSeg, 3, 0);
    lv_obj_set_style_pad_column(gSeg, 3, 0);
    lv_obj_set_style_bg_color(gSeg, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(gSeg, UI_COL_BORDER, 0);
    lv_obj_set_style_bg_color(gSeg, UI_COL_BORDER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(gSeg, UI_COL_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(gSeg, onSeg, LV_EVENT_VALUE_CHANGED, nullptr);
    ui_no_scroll(gSeg);

    for (lv_obj_t *&sec : gSec) sec = ui_section(tab);
    buildInfo(gSec[0]);
    ui_display_tab(gSec[1]);
    ui_touch_tab(gSec[2]);
    ui_led_tab(gSec[3]);
    ui_sound_tab(gSec[4]);
    ui_io_tab(gSec[5]);
    for (int i = 1; i < SECTIONS; i++) lv_obj_add_flag(gSec[i], LV_OBJ_FLAG_HIDDEN);
}
