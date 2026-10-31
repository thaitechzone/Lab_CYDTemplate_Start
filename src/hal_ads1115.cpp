#include <Wire.h>

#include "app_config.h"
#include "board_config.h"
#include "hal.h"

// =====================================================================
//  ADS1115 (ADC 16 บิต 4 ช่อง I2C) -- อ่าน A0-A3 แบบ single-ended (เทียบกับ GND)
//  ใช้โหมด single-shot วนทีละช่อง แบบ non-blocking: เริ่มแปลง -> (ผ่านไปหลาย ms) -> เช็คบิต OS -> อ่านผล -> ช่องถัดไป
//  PGA = +-4.096 V (1 LSB = 125 uV)  *อย่าป้อนแรงดันเกิน VDD + 0.3 V ของ ADS1115*
// =====================================================================

namespace hal {

namespace {

constexpr uint8_t REG_CONVERSION = 0x00;
constexpr uint8_t REG_CONFIG = 0x01;
constexpr float LSB_VOLTS = 4.096f / 32768.0f;

bool gPresent = false;
bool gBusy = false;  // true = กำลังรอแปลงช่อง gChannel
int gChannel = 0;
uint32_t gStartedAt = 0;
uint32_t gLastTry = 0;
uint32_t gSweeps = 0;
int16_t gRaw[4] = {0, 0, 0, 0};

// OS=1 (เริ่มแปลง) | MUX=1xx (AINx เทียบ GND) | PGA=001 (+-4.096V) | MODE=1 (single-shot) | DR=101 (250 SPS) | COMP_QUE=11 (ปิด comparator)
uint16_t configWord(int ch)
{
    return 0x8000 | static_cast<uint16_t>((4 + ch) << 12) | (0x1 << 9) | 0x0100 | (0x5 << 5) | 0x0003;
}

bool writeReg(uint8_t reg, uint16_t value)
{
    Wire.beginTransmission(cyd::ADS1115_ADDR);
    Wire.write(reg);
    Wire.write(static_cast<uint8_t>(value >> 8));
    Wire.write(static_cast<uint8_t>(value & 0xFF));
    return Wire.endTransmission() == 0;
}

bool readReg(uint8_t reg, uint16_t &value)
{
    Wire.beginTransmission(cyd::ADS1115_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom(static_cast<uint16_t>(cyd::ADS1115_ADDR), static_cast<uint8_t>(2)) != 2) return false;
    const uint8_t hi = Wire.read();
    const uint8_t lo = Wire.read();
    value = static_cast<uint16_t>((hi << 8) | lo);
    return true;
}

bool startConversion(uint32_t now)
{
    if (!writeReg(REG_CONFIG, configWord(gChannel))) return false;
    gBusy = true;
    gStartedAt = now;
    return true;
}

void lost()
{
    gPresent = false;
    gBusy = false;
    Serial.println(F("[ADS] lost connection"));
}

}  // namespace

bool adsBegin()
{
    gChannel = 0;
    gPresent = startConversion(millis());
    Serial.printf("[ADS] address 0x%02X %s\n", cyd::ADS1115_ADDR, gPresent ? "found" : "NOT FOUND");
    return gPresent;
}

void adsPoll(uint32_t now)
{
    if (!gPresent) {  // หายไป -> ลองต่อใหม่เป็นระยะ
        if (now - gLastTry < app::ADS_RETRY_MS) return;
        gLastTry = now;
        gChannel = 0;
        if (startConversion(now)) {
            gPresent = true;
            Serial.println(F("[ADS] connected"));
        }
        return;
    }
    if (!gBusy) {
        if (!startConversion(now)) lost();
        return;
    }
    if (now - gStartedAt < 3) return;  // 250 SPS = 4 ms ต่อครั้ง

    uint16_t cfg;
    if (!readReg(REG_CONFIG, cfg)) {
        lost();
        return;
    }
    if (!(cfg & 0x8000)) {  // OS = 0 -> ยังแปลงไม่เสร็จ
        if (now - gStartedAt > 100) lost();
        return;
    }
    uint16_t value;
    if (!readReg(REG_CONVERSION, value)) {
        lost();
        return;
    }
    gRaw[gChannel] = static_cast<int16_t>(value);
    gBusy = false;
    gChannel = (gChannel + 1) & 3;
    if (gChannel == 0) gSweeps++;
    if (!startConversion(now)) lost();
}

bool adsPresent()
{
    return gPresent;
}

int16_t adsRaw(int ch)
{
    return gRaw[ch & 3];
}

float adsVolts(int ch)
{
    const float v = gRaw[ch & 3] * LSB_VOLTS;
    return v < 0 ? 0 : v;  // single-ended: ค่าติดลบเล็กน้อยคือ offset/noise
}

uint32_t adsSweeps()
{
    return gSweeps;
}

}  // namespace hal
