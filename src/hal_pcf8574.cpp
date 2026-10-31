#include <Wire.h>

#include "app_config.h"
#include "board_config.h"
#include "hal.h"

// =====================================================================
//  PCF8574 (I/O expander I2C 8 บิต แบบ quasi-bidirectional)
//    P0-P3 = OUTPUT ต่อ LED แบบ Isink  : เขียน 0 = ขาจมกระแส = LED ติด, เขียน 1 = ดับ
//    P4-P7 = INPUT  ต่อสวิตช์ Pull-up 10k Active Low : ต้องเขียน 1 ค้างไว้ที่ขา input เสมอ
//            อ่านได้ 0 = กดสวิตช์, 1 = ปล่อย
//  ฝั่งผู้เรียกใช้ "ตรรกะ" ล้วน: LED บิต 1 = ติด, สวิตช์บิต 1 = กด  (การกลับขั้วอยู่ในไฟล์นี้ที่เดียว)
// =====================================================================

namespace hal {

namespace {

constexpr uint8_t INPUT_PINS = 0xF0;  // P4-P7 เขียน 1 ค้างไว้ (ปล่อยขาให้ pull-up ดึง)

bool gPresent = false;
uint8_t gLeds = 0;       // LED ที่ต้องการ บิต 0-3 (1 = ติด)
uint8_t gRaw = 0xFF;     // ค่าที่อ่านจากชิปล่าสุด (ระดับขาจริง)
uint8_t gSwitches = 0;   // สวิตช์ที่กด (หลัง debounce) บิต 0-3 = SW0-SW3 = P4-P7
uint8_t gCandidate = 0;  // ค่าที่เห็นล่าสุด รอให้นิ่งก่อนนับ
uint8_t gStable = 0;
uint32_t gLastPoll = 0;
uint32_t gLastTry = 0;

uint8_t outputByte()
{
    return INPUT_PINS | (static_cast<uint8_t>(~gLeds) & 0x0F);
}

bool writeOutputs()
{
    Wire.beginTransmission(cyd::PCF8574_ADDR);
    Wire.write(outputByte());
    return Wire.endTransmission() == 0;
}

bool readPins(uint8_t &out)
{
    if (Wire.requestFrom(static_cast<uint16_t>(cyd::PCF8574_ADDR), static_cast<uint8_t>(1)) != 1) return false;
    out = static_cast<uint8_t>(Wire.read());
    return true;
}

void lost()
{
    gPresent = false;
    gSwitches = 0;
    gStable = 0;
    gCandidate = 0;
    Serial.println(F("[PCF] lost connection"));
}

}  // namespace

bool ioBegin()
{
    Wire.begin(cyd::I2C_PIN_SDA, cyd::I2C_PIN_SCL, app::I2C_FREQ_HZ);
    Wire.setTimeOut(20);
    gPresent = writeOutputs();  // เขียนค่าเริ่มต้น: LED ดับ + ขา input ปล่อย
    Serial.printf("[PCF] address 0x%02X SDA=%d SCL=%d %s\n", cyd::PCF8574_ADDR, cyd::I2C_PIN_SDA, cyd::I2C_PIN_SCL, gPresent ? "found" : "NOT FOUND");
    return gPresent;
}

void ioPoll(uint32_t now)
{
    if (now - gLastPoll < app::IO_POLL_MS) return;
    gLastPoll = now;

    if (!gPresent) {  // หายไป -> ลองใหม่เป็นระยะ (เสียบสายคืนแล้วใช้งานต่อได้เอง)
        if (now - gLastTry < app::IO_RETRY_MS) return;
        gLastTry = now;
        if (writeOutputs()) {
            gPresent = true;
            Serial.println(F("[PCF] connected"));
        }
        return;
    }

    uint8_t raw;
    if (!readPins(raw)) {
        lost();
        return;
    }
    gRaw = raw;

    // debounce: ต้องเห็นค่าเดิมติดกัน IO_DEBOUNCE_SAMPLES ครั้งจึงเปลี่ยนสถานะสวิตช์
    const uint8_t pressed = static_cast<uint8_t>(~(raw >> 4)) & 0x0F;  // Active Low -> กลับเป็น 1 = กด
    static uint8_t same = 0;
    if (pressed == gCandidate) {
        if (same < 255) same++;
    } else {
        gCandidate = pressed;
        same = 1;
    }
    if (same >= app::IO_DEBOUNCE_SAMPLES) gStable = gCandidate;
    gSwitches = gStable;
}

bool ioPresent()
{
    return gPresent;
}

void ioSetLeds(uint8_t mask)
{
    mask &= 0x0F;
    if (mask == gLeds && gPresent) return;
    gLeds = mask;
    if (gPresent && !writeOutputs()) lost();
}

uint8_t ioLeds()
{
    return gLeds;
}

void ioToggleLed(int index)
{
    if (index < 0 || index > 3) return;
    ioSetLeds(gLeds ^ (1u << index));
}

uint8_t ioSwitches()
{
    return gSwitches;
}

uint8_t ioRaw()
{
    return gRaw;
}

}  // namespace hal
