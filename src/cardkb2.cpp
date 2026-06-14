#ifdef USE_CARDKB2
#include "cardkb2.h"
#include "idf/launcher_platform.h"
#include "powerSave.h"
#include <M5Unified.h>
#include <globals.h>

bool CardKB2Installed = false;
volatile bool CardKB2TextMode = false;

static constexpr uint8_t CARDKB2_I2C_ADDR = 0x5F;
static constexpr uint32_t CARDKB2_I2C_FREQ = 100000; // keyboard firmware runs the bus at 100kHz

// ASCII codes sent in I2C mode (press events only; arrows are not sent in this mode)
static constexpr uint8_t CARDKB2_KEY_BACKSPACE = 0x08;
static constexpr uint8_t CARDKB2_KEY_ENTER = 0x0A; // LF (the original CardKB sent CR)
static constexpr uint8_t CARDKB2_KEY_ESC = 0x1B;   // Fn+1

/*********************************************************************
** Function: cardkb2_setup
** location: _post_setup_gpio() of boards with a free Grove port
** Probes the Grove I2C bus for a CardKB2 and remembers the result
**********************************************************************/
bool cardkb2_setup() {
    M5.Power.setExtOutput(true); // Grove 5V feeds the keyboard
    // The keyboard's own ESP32-C61 may still be booting when we get here
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) launcherDelayMs(150);
        if (M5.Ex_I2C.begin() && M5.Ex_I2C.scanID(CARDKB2_I2C_ADDR, CARDKB2_I2C_FREQ)) {
            CardKB2Installed = true;
            launcherConsolePrintf("CardKB2 detected on Grove I2C (0x%02X)\n", CARDKB2_I2C_ADDR);
            return true;
        }
    }
    M5.Ex_I2C.release();
    return false;
}

// A plain 1-byte read (no register) pops one key from the keyboard's
// 32-deep FIFO; 0x00 means the FIFO is empty
static uint8_t cardkb2_read_key() {
    uint8_t key = 0;
    if (M5.Ex_I2C.start(CARDKB2_I2C_ADDR, true, CARDKB2_I2C_FREQ)) {
        M5.Ex_I2C.read(&key, 1, true);
        M5.Ex_I2C.stop();
    }
    return key;
}

/*********************************************************************
** Function: cardkb2_poll
** location: InputHandler() of boards with a free Grove port
** Drains pending keys and feeds the global input state
**********************************************************************/
void cardkb2_poll() {
    if (!CardKB2Installed) return;

    uint8_t batch[16];
    size_t count = 0;
    while (count < sizeof(batch)) {
        uint8_t key = cardkb2_read_key();
        if (key == 0) break;
        // The firmware can emit spurious bytes (e.g. 0x01); accept only documented values
        bool valid = key == CARDKB2_KEY_BACKSPACE || key == CARDKB2_KEY_ENTER || key == 0x0D ||
                     key == CARDKB2_KEY_ESC || (key >= 0x20 && key <= 0x7E);
        if (valid) batch[count++] = key;
    }
    if (count == 0) return;

    if (wakeUpScreen()) { // a key pressed while the screen sleeps only wakes it
        AnyKeyPress = true;
        return;
    }

    KeyStroke.Clear();
    for (size_t i = 0; i < count; ++i) {
        switch (batch[i]) {
            case CARDKB2_KEY_ENTER:
            case 0x0D:
                KeyStroke.enter = true;
                if (!CardKB2TextMode) SelPress = true;
                break;
            case CARDKB2_KEY_ESC:
                KeyStroke.exit_key = true;
                if (!CardKB2TextMode) EscPress = true;
                break;
            case CARDKB2_KEY_BACKSPACE:
                KeyStroke.del = true;
                if (!CardKB2TextMode) EscPress = true;
                break;
            default:
                KeyStroke.word.push_back((char)batch[i]);
                if (!CardKB2TextMode) {
                    // I2C mode sends no arrow codes: navigate with the keys
                    // carrying the arrow legends on the CardKB2 keycaps
                    if (batch[i] == 'd') UpPress = true;
                    else if (batch[i] == 'x') DownPress = true;
                    else if (batch[i] == 'z') PrevPress = true;
                    else if (batch[i] == 'c') NextPress = true;
                }
                break;
        }
    }
    KeyStroke.pressed = true;
    AnyKeyPress = true;
}
#endif // USE_CARDKB2
