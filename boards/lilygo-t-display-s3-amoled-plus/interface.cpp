#include "powerSave.h"
#include <PowersBQ25896.tpp>
static PowersBQ25896 PMU;
static bool PMU_OK = false;
#include <TouchDrv.hpp>
#include <interface.h>
#define BOARD_I2C_SDA 3
#define BOARD_I2C_SCL 2
#define BOARD_SENSOR_IRQ 21
#define BOARD_TOUCH_RST 16
TouchDrvCST816 touch;

void touchHomeKeyCallback(void *user_data) {
    Serial.println("Home key pressed!");
    static uint32_t checkMs = 0;
    if (millis() > checkMs) {
        EscPress = true;
        AnyKeyPress = true;
        wakeUpScreen();
    }
    checkMs = millis() + 200;
}

/***************************************************************************************
** Function name: _setup_gpio()
** Location: main.cpp
** Description:   initial setup for the device
***************************************************************************************/
void _setup_gpio() {
    pinMode(BOARD_TOUCH_RST, OUTPUT); // PIN_TOUCH_RES
    pinMode(38 /* PMIC_EN */, OUTPUT);

    digitalWrite(38 /* PMIC_EN */, HIGH);
    digitalWrite(BOARD_TOUCH_RST, LOW); // PIN_TOUCH_RES
    delay(100);
    digitalWrite(BOARD_TOUCH_RST, HIGH); // PIN_TOUCH_RES

    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL); // SDA, SCL

    // Initialize capacitive touch
    touch.setPins(BOARD_TOUCH_RST, BOARD_SENSOR_IRQ);
    touch.begin(Wire, CST816_SLAVE_ADDRESS, BOARD_I2C_SDA, BOARD_I2C_SCL);
    // Set the screen to turn on or off after pressing the screen Home touch button
    touch.setCenterButtonCoordinate(600, 120);
    touch.setHomeButtonCallback(touchHomeKeyCallback, NULL);

    bool hasPMU = PMU.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, BQ25896_SLAVE_ADDRESS);
    if (!hasPMU) {
        Serial.println("PMU is not online...");
    } else {
        PMU_OK = true;
        PMU.disableOTG();
        PMU.enableMeasure();
        PMU.enableCharge();
    }
}

/***************************************************************************************
** Function name: getBattery()
** location: display.cpp
** Description:   Delivers the battery value from 1-100
***************************************************************************************/
int getBattery() {
    int percent = 0;
    if (PMU_OK) percent = PMU.getNTCPercentage();
    return (percent < 0) ? 0 : (percent >= 100) ? 100 : percent;
}

/*********************************************************************
** Function: setBrightness
** location: settings.cpp
** set brightness value
**********************************************************************/
void _setBrightness(uint8_t brightval) {
    extern Arduino_DataBus *bus;
    bus->beginWrite();
    bus->writeC8D8(0x51, (brightval * 255) / 100);
    bus->endWrite();
}

struct LTouchPointPro {
    int16_t x = 0;
    int16_t y = 0;
};
/*********************************************************************
** Function: InputHandler
** Handles the variables PrevPress, NextPress, SelPress, AnyKeyPress and EscPress
**********************************************************************/
void InputHandler(void) {
    static unsigned long _tmptmp = 0;
    LTouchPointPro t;
    uint8_t touched = 0;
    uint8_t rot = 5;
    if (rot != rotation) {
        if (rotation == 0) {
            touch.setMaxCoordinates(TFT_WIDTH, TFT_HEIGHT);
            touch.setSwapXY(true);
            touch.setMirrorXY(true, false);
        }
        if (rotation == 2) {
            touch.setMaxCoordinates(TFT_WIDTH, TFT_HEIGHT);
            touch.setSwapXY(true);
            touch.setMirrorXY(false, true);
        }
        if (rotation == 1) {
            touch.setMaxCoordinates(TFT_HEIGHT, TFT_WIDTH);
            touch.setSwapXY(false);
            touch.setMirrorXY(false, false);
        }
        if (rotation == 3) {
            touch.setMaxCoordinates(TFT_HEIGHT, TFT_WIDTH);
            touch.setSwapXY(false);
            touch.setMirrorXY(true, true);
        }
        rot = rotation;
    }
    touched = touch.getPoint(&t.x, &t.y, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    if ((millis() - _tmptmp) > 200 || LongPress) { // one reading each 500ms

        // Serial.printf("\nPressed x=%d , y=%d, rot: %d",t.x, t.y, rotation);
        if (touched) {

            Serial.printf(
                "\nPressed x=%d , y=%d, rot: %d, millis=%d, tmp=%d", t.x, t.y, rotation, millis(), _tmptmp
            );
            _tmptmp = millis();

            if (!wakeUpScreen()) AnyKeyPress = true;
            else return;

            // Touch point global variable
            touchPoint.x = t.x;
            touchPoint.y = t.y;
            touchPoint.pressed = true;
            touchHeatMap(touchPoint);
        }
    }
}
