#include "IoExpanderXL9555.hpp"
#include "idf/idf_wifi.h"
#include "idf/launcher_platform.h"
#include "powerSave.h"
#include <SD_MMC.h>
#include <interface.h>

IoExpanderXL9555 io;
#define SENSOR_IRQ 5 // XL9535 IRQ
void _setup_gpio() {
    Serial.println("Start GPIO");

    // Inicializar SDMMC pins
    SD_MMC.setPins(SDIO_1_CLK, SDIO_1_CMD, SDIO_1_D0, SDIO_1_D1, SDIO_1_D2, SDIO_1_D3);
    // Inicializar GPIO
    pinMode(SENSOR_IRQ, INPUT_PULLUP);
    Serial.println("2");
    // Inicializar IO Expander
    const uint8_t chip_address = XL9555_UNKNOWN_ADDRESS;
    Wire.begin(IIC_1_SDA, IIC_1_SCL);
    if (!io.begin(Wire, chip_address, IIC_1_SDA, IIC_1_SCL)) {
        while (1) {
            Serial.println("Failed to find XL9555 - check your wiring!");
            delay(1000);
        }
    }
    // io.configPins(IoExpanderXL9555::PORT_ALL, INPUT);
    /*
    inline constexpr auto kPowerEn3v3 = cpp_bus_driver::Xl95x5::Pin::kIo0;
    inline constexpr auto kSky13453Vctl = cpp_bus_driver::Xl95x5::Pin::kIo1;
    inline constexpr auto kScreenRst = cpp_bus_driver::Xl95x5::Pin::kIo2;
    inline constexpr auto kTouchRst = cpp_bus_driver::Xl95x5::Pin::kIo3;
    inline constexpr auto kTouchInt = cpp_bus_driver::Xl95x5::Pin::kIo4;
    inline constexpr auto kEthernetRst = cpp_bus_driver::Xl95x5::Pin::kIo5;
    inline constexpr auto kAudioPowerEn = cpp_bus_driver::Xl95x5::Pin::kIo6;
    inline constexpr auto kIcm20948Int = cpp_bus_driver::Xl95x5::Pin::kIo7;
    inline constexpr auto kUsbPhyPowerEn = cpp_bus_driver::Xl95x5::Pin::kIo10;
    inline constexpr auto kGpsWakeUp = cpp_bus_driver::Xl95x5::Pin::kIo11;
    inline constexpr auto kRtcInt = cpp_bus_driver::Xl95x5::Pin::kIo12;
    inline constexpr auto kEsp32c6WakeUp = cpp_bus_driver::Xl95x5::Pin::kIo13;
    inline constexpr auto kEsp32c6En = cpp_bus_driver::Xl95x5::Pin::kIo14;
    inline constexpr auto kSdPowerEn = cpp_bus_driver::Xl95x5::Pin::kIo15;
    inline constexpr auto kRadioRst = cpp_bus_driver::Xl95x5::Pin::kIo16;
    inline constexpr auto kRadioDio1 = cpp_bus_driver::Xl95x5::Pin::kIo17;
    inline constexpr auto kSx1262Rst = kRadioRst;
    inline constexpr auto kSx1262Dio1 = kRadioDio1;
    inline constexpr auto kLr2021Rst = kRadioRst;
    inline constexpr auto kLr2021Dio1 = kRadioDio1;

    */
    const uint8_t expands[] = {
        0,  // kPowerEn3v3
        2,  // kScreenRst
        3,  // kTouchRst
        5,  // kEthernetRst
        6,  // kAudioPowerEn
        13, // kEsp32c6WakeUp
        14, // kEsp32c6En
        15  // kSdPowerEn
    };
    for (auto pin : expands) {
        // io.pinMode(pin, OUTPUT);
        // io.digitalWrite(pin, HIGH);
        Serial.printf("Pin %d set HIGH", pin);
        delay(1);
    }
    io.pinMode(4, INPUT); // kTouchInt

    delay(300);
    Serial.println("Starting WIFI Hosted");
    // launcherWifiInitHostedSdio(SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST);
    Serial.println("Finish setup GPIO");
}

void _post_setup_gpio() {}

void _setBrightness(uint8_t brightval) {
    Wire.beginTransmission(0x51);
    Wire.write((brightval * 255) / 100);
    Wire.endTransmission();
}

void InputHandler(void) {
    if (launcherGpioRead(SEL_BTN) == BTN_ACT) {
        SelPress = true;
        AnyKeyPress = true;
    }
}
