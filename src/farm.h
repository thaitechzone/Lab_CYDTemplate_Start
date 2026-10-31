#pragma once

// =====================================================================
//  farm.h -- ตรรกะระบบ Smart Farm (ไม่รู้จัก LVGL)
//    เซนเซอร์ = ช่อง ADC ของ ADS1115 (A0-A3) แปลงเป็น % ของ 0 - ADC_FULLSCALE_V
//    อุปกรณ์   = LED0-LED3 ของ PCF8574 (ปั๊มน้ำ พัดลม ไฟปลูก เตือนภัย)
//    สวิตช์    = SW0-SW3 ของ PCF8574
//    กฎ (Rule) = "ถ้าเงื่อนไข (ADC หรือ SW) ... [และ/หรือ เงื่อนไขที่ 2] -> ให้อุปกรณ์ ติด/ดับ" ผู้ใช้กำหนดเองได้ 6 กฎ
//  engine ทำงานใน loop() ทุกแท็บ ไม่ขึ้นกับว่าหน้าจอเปิดหน้าไหนอยู่
// =====================================================================

#include <Arduino.h>

namespace farm {

constexpr int MAX_RULES = 6;
constexpr int N_OUT = 4;
constexpr int N_ADC = 4;
constexpr int N_SW = 4;

enum Src : uint8_t { SRC_NONE = 0, SRC_ADC0 = 1, SRC_SW0 = 5 };  // 1-4 = A0-A3, 5-8 = SW0-SW3
enum AdcOp : uint8_t { ADC_GREATER = 0, ADC_LESS = 1 };
enum SwOp : uint8_t { SW_PRESSED = 0, SW_RELEASED = 1, SW_TOGGLE = 2 };  // กดอยู่ / ปล่อย / กดสลับ (ทุกครั้งที่กด สถานะสลับ)
enum Combine : uint8_t { COMB_NONE = 0, COMB_AND = 1, COMB_OR = 2 };

struct Cond {
    uint8_t src = SRC_NONE;
    uint8_t op = 0;
    uint8_t value = 50;  // % ของเต็มสเกล (ใช้กับ ADC)
};

struct Rule {
    bool enabled = false;
    Cond c1, c2;
    uint8_t combine = COMB_NONE;
    uint8_t out = 0;            // อุปกรณ์ 0-3
    bool onWhenTrue = true;     // true = เงื่อนไขจริงแล้ว "ติด", false = เงื่อนไขจริงแล้ว "ดับ"
    uint8_t holdSec = 0;        // เงื่อนไขหมดแล้วยังคงสถานะต่ออีกกี่วินาที
};

const char *sensorName(int ch);
const char *outputName(int out);

void begin();                 // โหลดกฎ + โหมด จาก NVS (ยังไม่มี = ใช้กฎตัวอย่าง)
void tick(uint32_t now);      // เรียกจาก loop() บ่อยๆ

Rule &rule(int i);
void saveRules();
void resetRules();            // คืนกฎตัวอย่างของสวน
void clearRule(int i);

bool autoMode();
void setAutoMode(bool on);

float sensorPct(int ch);                 // % ของเต็มสเกล (กรองแล้ว)
bool condNow(const Cond &c);             // ผลของเงื่อนไขตอนนี้ (ไว้แสดงตัวอย่างตอนแก้กฎ ไม่มี hysteresis)
bool ruleActive(int i);                  // กฎนี้ "เป็นจริง" อยู่ (รวมช่วงค้าง)
bool ruleControlled(int out);            // มีกฎที่เปิดอยู่ควบคุมอุปกรณ์นี้
bool overridden(int out);                // ผู้ใช้สั่งมือแทรกอยู่ (จะหายเมื่อกฎเปลี่ยนสถานะครั้งถัดไป)
bool manualToggle(int out);              // สั่งมือ (คืนสถานะใหม่)

uint32_t eventSeq();                     // เพิ่มทุกครั้งที่อุปกรณ์เปลี่ยนสถานะ
const char *eventText();                 // ข้อความเหตุการณ์ล่าสุด

}  // namespace farm
