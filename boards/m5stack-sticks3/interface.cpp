#include "idf/launcher_platform.h"
#include "powerSave.h"
#include <M5Unified.h>
#include <Wire.h>
#include <interface.h>

/***************************************************************************************
** Function name: _setup_gpio()
** Location: main.cpp
** Description:   initial setup for the device
***************************************************************************************/
void _setup_gpio() {
    M5.begin();
    M5.Power.setExtOutput(false);
    // Disable 5V output to external port
    /*
  | Device  | SCK   | MISO  | MOSI  | CS    | GDO0/CE   |
  | ---     | :---: | :---: | :---: | :---: | :---:     |
  | SD Card | 5     | 4     | 6     | 7     | ---       |
  | CC1101  | 5     | 4     | 6     | 2     | 3         |
  | NRF24   | 5     | 4     | 6     | 8     | 1         |
  | PN532   | 5     | 4     | 6     | 43    | --        |
  | WS500   | 5     | 4     | 6     | **    | **        |
  | LoRa    | 5     | 4     | 6     | **    | **        |
      */
    launcherGpioOutput(7);
    launcherGpioWrite(7, HIGH); // SD Card CS
    launcherGpioOutput(2);
    launcherGpioWrite(2, HIGH); // CC1101 CS
    launcherGpioOutput(8);
    launcherGpioWrite(8, HIGH); // nRF24L01 CS
    launcherGpioOutput(43);
    launcherGpioWrite(43, HIGH); // PN532 CS
    launcherGpioOutput(9);
    launcherGpioWrite(9, LOW); // M5RF433 avoid Jamming
    launcherGpioOutput(46);
    launcherGpioWrite(46, LOW); // Infrared LED Off

    M5.BtnA.setDebounceThresh(8);
    M5.BtnB.setDebounceThresh(8);
    M5.BtnB.setHoldThresh(500);
}
/*********************************************************************
** Function: setBrightness
** location: settings.cpp
** set brightness value
**********************************************************************/
void _setBrightness(uint8_t brightval) { M5.Display.setBrightness(brightval); }

/***************************************************************************************
** Function name: getBattery()
** location: display.cpp
** Description:   Delivers the battery value from 1-100
***************************************************************************************/
int getBattery() {
    static int lastState = -1;
    bool charging = M5.Power.isCharging();
    if (charging && lastState != 1) {
        lastState = 1;
        M5.Power.setExtOutput(false);
    } else if (!charging && lastState != 0) {
        lastState = 0;
        M5.Power.setExtOutput(true);
    }
    int level = M5.Power.getBatteryLevel();
    return (level < 0) ? 0 : (level >= 100) ? 100 : level;
}

/*********************************************************************
** Function: InputHandler
** Handles the variables PrevPress, NextPress, SelPress, AnyKeyPress and EscPress
**********************************************************************/
void InputHandler(void) {
    M5.update();

    bool btnAActive = M5.BtnA.isPressed() || M5.BtnA.isHolding();
    bool btnBActive = M5.BtnB.isPressed() || M5.BtnB.isHolding();
    bool hasEvent =
        M5.BtnA.wasPressed() || M5.BtnB.wasHold() || M5.BtnB.wasSingleClicked() || M5.BtnB.wasDoubleClicked();

    AnyKeyPress = btnAActive || btnBActive || hasEvent;
    if (!AnyKeyPress) return;

    if (!wakeUpScreen()) AnyKeyPress = true;
    else return;

    if (M5.BtnA.wasPressed()) SelPress = true;
    if (M5.BtnB.wasSingleClicked()) NextPress = true;
    if (M5.BtnB.wasDoubleClicked()) PrevPress = true;
    if (M5.BtnB.wasHold()) EscPress = true;
}

/*********************************************************************
** Function: powerOff
** location: mykeyboard.cpp
** Turns off the device (or try to)
**********************************************************************/
void powerOff() { M5.Power.powerOff(); }
