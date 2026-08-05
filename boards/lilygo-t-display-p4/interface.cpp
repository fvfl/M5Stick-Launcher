#include "GaugeBQ27220.hpp"
#include "IoExpanderXL9555.hpp"
#include "idf/idf_wifi.h"
#include "idf/launcher_platform.h"
#include "powerSave.h"
#include <SD_MMC.h>
#include <TouchDrv.hpp>
#include <esp_sleep.h>
#include <interface.h>

IoExpanderXL9555 io;
GaugeBQ27220 gauge;
// The TFT build carries an HI8561 (touch integrated with the display driver,
// reports native panel coordinates); the AMOLED build carries a GT9895 (separate
// controller with its own 1060x2400 grid). Both derive from TouchDrvInterface,
// so only the construction and configuration differ - the read path is shared.
#if defined(TOUCH_GT9895)
TouchDrvGT9895 touch;
#else
TouchDrvHI8561 touch;
#endif
static bool touchOk = false;
static bool ioOk = false;
static bool gaugeOk = false;
#define SENSOR_IRQ 5 // XL9535 IRQ

enum : uint8_t {
    XL_POWER_EN_3V3 = 0,     // kIo0  - active LOW
    XL_SKY13453_VCTL = 1,    // kIo1  - antenna select, 1 == RF1
    XL_SCREEN_RST = 2,       // kIo2
    XL_TOUCH_RST = 3,        // kIo3
    XL_TOUCH_INT = 4,        // kIo4  - input
    XL_ETHERNET_RST = 5,     // kIo5
    XL_POWER_EN_5V0 = 6,     // kIo6
    XL_ICM20948_INT = 7,     // kIo7  - input
    XL_USB_PHY_POWER_EN = 8, // kIo10
    XL_GPS_WAKEUP = 9,       // kIo11
    XL_RTC_INT = 10,         // kIo12
    XL_ESP32C6_WAKEUP = 11,  // kIo13
    XL_ESP32C6_EN = 12,      // kIo14
    XL_SD_POWER_EN = 13,     // kIo15 - active LOW
    XL_SX1262_RST = 14,      // kIo16
    XL_SX1262_DIO1 = 15,     // kIo17 - input
};

// Reproduces ConfigXl9535() from the LilyGO reference driver
// (t_display_p4_driver.cpp). The rail enables are active LOW and the peripheral
// resets are toggled twice, so the final levels are not all "HIGH" - driving
// kSdPowerEn HIGH keeps the SD card powered off.
static void _config_xl9535() {
    const uint8_t outputs[] = {
        XL_SCREEN_RST,
        XL_TOUCH_RST,
        XL_USB_PHY_POWER_EN,
        XL_POWER_EN_5V0,
        XL_POWER_EN_3V3,
        XL_GPS_WAKEUP,
        XL_ESP32C6_EN,
        XL_ETHERNET_RST,
        XL_SD_POWER_EN,
        XL_SX1262_RST,
        XL_SKY13453_VCTL,
    };
    for (auto pin : outputs) io.pinMode(pin, OUTPUT);
    io.pinMode(XL_ICM20948_INT, INPUT);
    io.pinMode(XL_SX1262_DIO1, INPUT);
    io.pinMode(XL_TOUCH_INT, INPUT);

    io.digitalWrite(XL_USB_PHY_POWER_EN, LOW);
    io.digitalWrite(XL_SKY13453_VCTL, HIGH); // RF1 antenna

    io.digitalWrite(XL_POWER_EN_3V3, LOW);
    delay(10);
    io.digitalWrite(XL_POWER_EN_3V3, HIGH);
    delay(500);
    io.digitalWrite(XL_POWER_EN_3V3, LOW);
    delay(10);

    // Two reset pulses, exactly as the vendor driver does.
    for (int pass = 0; pass < 3; pass++) {
        const uint8_t level = (pass == 1) ? LOW : HIGH;
        io.digitalWrite(XL_SCREEN_RST, level);
        io.digitalWrite(XL_TOUCH_RST, level);
        io.digitalWrite(XL_ESP32C6_EN, level);
        io.digitalWrite(XL_ETHERNET_RST, level);
        io.digitalWrite(XL_GPS_WAKEUP, level);
        io.digitalWrite(XL_SX1262_RST, level);
        io.digitalWrite(XL_SD_POWER_EN, level == HIGH ? LOW : HIGH);
        io.digitalWrite(XL_POWER_EN_5V0, level == HIGH ? HIGH : LOW);
        delay(pass == 2 ? 120 : 10);
    }
}

void _setup_gpio() {
    Serial.println("Start GPIO");
    // Inicializar SDMMC pins
    SD_MMC.setPins(SDIO_1_CLK, SDIO_1_CMD, SDIO_1_D0, SDIO_1_D1, SDIO_1_D2, SDIO_1_D3);
    // Inicializar GPIO
    pinMode(SENSOR_IRQ, INPUT_PULLUP);
    pinMode(35, INPUT);
    Serial.println("2");
    // Inicializar IO Expander
    const uint8_t chip_address = XL9555_UNKNOWN_ADDRESS;
    Wire.begin(IIC_1_SDA, IIC_1_SCL);
    ioOk = io.begin(Wire, chip_address, IIC_1_SDA, IIC_1_SCL);
    if (!ioOk) { Serial.println("Failed to find XL9555 - check your wiring!"); }
    io.configPins(IoExpanderXL9555::PORT_ALL, INPUT);

    if (ioOk) {
        _config_xl9535();
        Serial.println("XL9535 configured (rails on, C6 out of reset)");
        const uint16_t port = io.digitalReadPort();
        Serial.printf(
            "XL9535 port=0x%04X | 3v3En=%d(exp0) 5v0En=%d(exp1) SdPwrEn=%d(exp0) C6En=%d(exp1) "
            "ScrRst=%d(exp1) TouchRst=%d(exp1)\n",
            port,
            (port >> XL_POWER_EN_3V3) & 1,
            (port >> XL_POWER_EN_5V0) & 1,
            (port >> XL_SD_POWER_EN) & 1,
            (port >> XL_ESP32C6_EN) & 1,
            (port >> XL_SCREEN_RST) & 1,
            (port >> XL_TOUCH_RST) & 1
        );
    }

    // BQ27220 fuel gauge, same I2C bus as the expander.
    gaugeOk = gauge.begin(Wire, IIC_1_SDA, IIC_1_SCL);
    Serial.println(gaugeOk ? "BQ27220 gauge started" : "BQ27220 gauge not found");

    delay(300);
    // A C6 still running LilyGO's factory firmware answers on SDIO but not with
    // the ESP-Hosted protocol, which makes the stack spin for ~20 s and then
    // panic - a boot loop. The guarded call notices that happened on the
    // previous boot and skips, leaving hostedWifiAvailable false so OTA/WebUI
    // can say so instead of hanging. After flashing the esp_hosted co-processor
    // firmware (see "Flashing the Coprocessor Network-Adapter Firmware" in the
    // T-Display-P4 README), 'wifi hosted retry' on the serial console re-arms it.
    Serial.println("Starting WIFI Hosted");
    if (!launcherWifiInitHostedSdioGuarded(
            SDIO2_CLK, SDIO2_CMD, SDIO2_D0, SDIO2_D1, SDIO2_D2, SDIO2_D3, SDIO2_RST
        )) {
        Serial.println("WIFI Hosted unavailable");
    }
    Serial.println("Finish setup GPIO");
}

// The HI8561's touch reset and interrupt lines hang off the XL9535, not off P4
// GPIOs, so SensorLib needs a way to reach them. It supports that through GPIO
// callbacks: pins with bit 7 set are routed to the expander, everything else
// falls through to the normal Arduino calls. Same convention the LilyGO
// TouchDrv_HI8561_GetPoint_LilyGoP4 example uses.
#define EXP_PIN(n) ((n) | 0x80)

static void _touchPinMode(uint8_t gpio, uint8_t mode) {
    if (gpio & 0x80) io.pinMode(gpio & 0x7F, mode);
    else pinMode(gpio, mode);
}

static void _touchDigitalWrite(uint8_t gpio, uint8_t level) {
    if (gpio & 0x80) io.digitalWrite(gpio & 0x7F, level);
    else digitalWrite(gpio, level);
}

static uint8_t _touchDigitalRead(uint8_t gpio) {
    if (gpio & 0x80) return io.digitalRead(gpio & 0x7F);
    return digitalRead(gpio);
}

void _post_setup_gpio() {
    // Touch has to come up *after* the panel: on this family the touch
    // controller shares silicon with the display driver and does not answer on
    // I2C until the display side is initialized. main.cpp calls tft->begin()
    // before _post_setup_gpio(), which is exactly the window we need.
    touch.setGpioCallback(_touchPinMode, _touchDigitalWrite, _touchDigitalRead);

#if defined(TOUCH_GT9895)
    // The GT9895 needs no reset line driven by us: the reference driver builds
    // it with rst = -1 and relies on the XL9535 sequence that already pulses
    // kTouchRst in _config_xl9535(). Only the interrupt is wired up here.
    touch.setPins(-1, EXP_PIN(XL_TOUCH_INT));
    touchOk = touch.begin(Wire, GT9895_SLAVE_ADDRESS_L, IIC_1_SDA, IIC_1_SCL);
#else
    touch.setPins(EXP_PIN(XL_TOUCH_RST), EXP_PIN(XL_TOUCH_INT));
    touchOk = touch.begin(Wire, HI8561_SLAVE_ADDRESS, IIC_1_SDA, IIC_1_SCL);
#endif
    if (!touchOk) {
        Serial.println("Touch controller not found");
        return;
    }

#if defined(TOUCH_GT9895)
    // The GT9895 digitizer grid is bigger than the panel, so the raw readings
    // have to be scaled instead of just clamped. Declaring the raw size first
    // lets setTargetResolution() work out the factors (the vendor driver hands
    // the same ratio to Gt9895 as kXScaleFactor / kYScaleFactor).
    touch.setResolution(TOUCH_RAW_WIDTH, TOUCH_RAW_HEIGHT);
    touch.setTargetResolution(TFT_WIDTH, TFT_HEIGHT);
#else
    // HI8561 already reports in panel coordinates - this only sets the clamp.
    touch.setMaxCoordinates(TFT_WIDTH, TFT_HEIGHT);
#endif
    Serial.printf("Touch started: %s (chip ID 0x%X)\n", touch.getModelName(), touch.getChipID());

    if (gaugeOk && gauge.refresh()) {
        Serial.printf("Battery: %d%% (%u mV, %d mA)\n", getBattery(), gauge.getVoltage(), gauge.getCurrent());
    }
}

#if defined(BACKLIGHT)
void _setBrightness(uint8_t brightval) { analogWrite(BACKLIGHT, (brightval * 255) / 100); }
#else
// The AMOLED has no backlight rail: brightness is the RM69A10's DCS 0x51
// (WRDISBV), which rm69a10_lcd_init_cmd sets to full at init. Driving it at
// runtime needs writeCommand(), which upstream Arduino_ESP32DSIPanel does not
// expose - support_files/patch_dsi_writecommand.py adds it at build time.
extern Arduino_ESP32DSIPanel *bus; // defined in display.cpp

void _setBrightness(uint8_t brightval) {
    if (bus == nullptr) return;
    // WRDISBV takes a single byte, 0x00..0xFF. The init sequence uses 0xFE for
    // "full", so the usual 0-100 scale maps straight onto 0-255.
    const uint8_t level = (uint8_t)((brightval * 255) / 100);
    bus->writeCommand(0x51, &level, 1);
}
#endif

void InputHandler(void) {
    if (launcherGpioRead(SEL_BTN) == BTN_ACT) {
        SelPress = true;
        AnyKeyPress = true;
        return;
    }

    if (!touchOk) return;

    static long lastRead = launcherMillis();
    if (launcherMillis() - lastRead <= 200 && !LongPress) return;
    lastRead = launcherMillis();

    const TouchPoints &points = touch.getTouchPoints();
    if (!points.hasPoints()) {
        touchPoint.pressed = false;
        return;
    }

    const TouchPoint &p = points.getPoint(0);

    if (wakeUpScreen()) return;
    AnyKeyPress = true;

    const uint16_t nx = p.x; // native, 0..TFT_WIDTH-1
    const uint16_t ny = p.y; // native, 0..TFT_HEIGHT-1
    uint16_t sx, sy;
    switch (rotation) {
        case 1:
            sx = ny;
            sy = (TFT_WIDTH - 1) - nx;
            break;
        case 2:
            sx = (TFT_WIDTH - 1) - nx;
            sy = (TFT_HEIGHT - 1) - ny;
            break;
        case 3:
            sx = (TFT_HEIGHT - 1) - ny;
            sy = nx;
            break;
        default: // rotation 0 - native orientation, confirmed working on hardware
            sx = nx;
            sy = ny;
            break;
    }

    Serial.printf(
        "Touch rot=%d native(%d,%d) -> screen(%d,%d) [%dx%d]\n",
        rotation,
        nx,
        ny,
        sx,
        sy,
        (rotation & 1) ? TFT_HEIGHT : TFT_WIDTH,
        (rotation & 1) ? TFT_WIDTH : TFT_HEIGHT
    );

    touchPoint.x = sx;
    touchPoint.y = sy;
    touchPoint.pressed = true;
    touchHeatMap(touchPoint);
}

/*********************************************************************
** Function: getBattery
** location: display.cpp
** Delivers the battery value from 0-100
**********************************************************************/
int getBattery() {
    if (!gaugeOk || !gauge.refresh()) return 0;
    int percent = gauge.getStateOfCharge();
    return (percent < 0) ? 0 : (percent > 100) ? 100 : percent;
}

static void _peripherals_power_down() {
    SD_MMC.end();
#if defined(BACKLIGHT)
    analogWrite(BACKLIGHT, 0);
#endif
    if (!ioOk) return;
    io.digitalWrite(XL_SCREEN_RST, LOW);
    io.digitalWrite(XL_TOUCH_RST, LOW);
    io.digitalWrite(XL_ESP32C6_EN, LOW);
    io.digitalWrite(XL_ETHERNET_RST, LOW);
    io.digitalWrite(XL_GPS_WAKEUP, LOW);
    io.digitalWrite(XL_SX1262_RST, LOW);
    // Both rail enables are active LOW, so HIGH is what switches them off.
    io.digitalWrite(XL_SD_POWER_EN, HIGH);
    io.digitalWrite(XL_POWER_EN_5V0, LOW);
    io.digitalWrite(XL_POWER_EN_3V3, HIGH);
}

void powerOff() {
    _setBrightness(0);
#if defined(BACKLIGHT)
    ledcDetach(BACKLIGHT);
    pinMode(BACKLIGHT, OUTPUT);
    digitalWrite(BACKLIGHT, LOW);
#endif
    _peripherals_power_down();

    delay(200);
    esp_deep_sleep_start();
}

void reboot() {
    // Cut the peripheral rails first and let them drain before the CPU resets.
    // The SD card is the reason: yanking the CPU mid-transaction leaves the
    // card powered and in an undefined state, and because a reset does not
    // power-cycle it, it can come back up refusing to initialize.
    _peripherals_power_down();
    delay(200);
    ESP.restart();
}
