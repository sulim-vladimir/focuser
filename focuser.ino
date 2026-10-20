// Focuser controller: Arduino Nano + DRV8825, 1/32 microstepping.
// Pins: STEP A0, DIR A1, SLP D5, M0-M2 D11/D10/D9, buttons in D3 / out D2.
//
// Serial 9600 baud. Two command sets:
//
// Legacy single characters (outside a ':' frame):
//   i / o   move in / out until 's'      h / l   high / low jog speed
//   s       stop
//
// Moonlite (":XX...#", positions are 4 hex digits, 0000-FFFF, larger = out):
//   :GP#  current position  -> "XXXX#"     :SPxxxx#  set current position (sync)
//   :GN#  target position   -> "XXXX#"     :SNxxxx#  set target position
//   :FG#  go to target                     :FQ#      stop
//   :GI#  moving            -> "01#"/"00#" :GV#      firmware version -> "10#"
//   :GD#  speed code        -> "XX#"       :SDxx#    speed code 02 (fast) .. 20 (slow)
//   :GH#  half step         -> "00#"       :GT#      temperature -> "0000#" (no sensor)
//   :GC#  temp coefficient  -> "00#"       :SH# :SF# :SCxx# :POxx# :C# :+# :-#  ignored
// Extensions:
//   :GK#  keep awake        -> "01#"/"00#" :SKxx#    00 = sleep driver when idle, 01 = hold
//
// The in/out buttons jog while held. Position is saved to EEPROM whenever the
// motor comes to rest (wear-levelled ring), keep-awake is saved when changed.

#include <AccelStepper.h>
#include <EEPROM.h>

#define M0  11
#define M1  10
#define M2  9
#define SLP 5
#define IN_BTN 3
#define OUT_BTN 2

#define POS_MIN 0L
#define POS_MAX 65535L
#define POS_DEFAULT 30000L      // first boot / empty EEPROM: room both ways
#define JOG_LOW_SPEED 50
#define JOG_HIGH_SPEED 200
#define ACCELERATION 300
#define STANDBY_MS 5000
#define DEBOUNCE_MS 20

#define SETTINGS_ADDR 0
#define SETTINGS_MAGIC 0xF5
#define RING_ADDR 16
#define RING_SLOTS 100

struct Settings {
    uint8_t magic;
    uint8_t keepAwake;
};

struct __attribute__((packed)) PosRecord {
    int32_t pos;
    uint32_t seq;
    uint8_t check;
};

AccelStepper motor(AccelStepper::DRIVER, A0, A1);
unsigned long prevCmdTime;
bool standby = false;
bool jogging = false;
bool keepAwake = false;
float jogSpeed = JOG_LOW_SPEED;
uint8_t speedCode = 0x04;
long targetPos;

uint8_t ringSlot;
uint32_t ringSeq;
long savedPos;

char frame[12];
uint8_t frameLen;
bool inFrame = false;

int8_t btnRaw = 0;
int8_t btnState = 0;
unsigned long btnChanged;

uint8_t recordCheck(const PosRecord &r)
{
    const uint8_t *p = (const uint8_t *)&r;
    uint8_t sum = 0x5A;
    for (uint8_t i = 0; i < offsetof(PosRecord, check); i++) {
        sum += p[i];
    }
    return sum;
}

long loadPosition()
{
    bool found = false;
    PosRecord r;
    for (uint8_t i = 0; i < RING_SLOTS; i++) {
        EEPROM.get(RING_ADDR + i * sizeof(PosRecord), r);
        if (r.check == recordCheck(r) && (!found || r.seq > ringSeq)) {
            found = true;
            ringSeq = r.seq;
            ringSlot = i;
            savedPos = r.pos;
        }
    }
    if (!found) {
        ringSeq = 0;
        ringSlot = RING_SLOTS - 1;
        savedPos = POS_DEFAULT;
    }
    return constrain(savedPos, POS_MIN, POS_MAX);
}

void savePosition(long pos)
{
    PosRecord r;
    r.pos = pos;
    r.seq = ++ringSeq;
    r.check = recordCheck(r);
    ringSlot = (ringSlot + 1) % RING_SLOTS;
    EEPROM.put(RING_ADDR + ringSlot * sizeof(PosRecord), r);
    savedPos = pos;
}

void loadSettings()
{
    Settings s;
    EEPROM.get(SETTINGS_ADDR, s);
    keepAwake = s.magic == SETTINGS_MAGIC && s.keepAwake == 1;
}

void saveSettings()
{
    Settings s = { SETTINGS_MAGIC, (uint8_t)(keepAwake ? 1 : 0) };
    EEPROM.put(SETTINGS_ADDR, s);
}

void setup() {
    pinMode(M0, OUTPUT);
    pinMode(M1, OUTPUT);
    pinMode(M2, OUTPUT);
    pinMode(IN_BTN, INPUT_PULLUP);
    pinMode(OUT_BTN, INPUT_PULLUP);

    digitalWrite(M0, LOW);
    digitalWrite(M1, HIGH);
    digitalWrite(M2, HIGH);

    pinMode(SLP, OUTPUT);
    digitalWrite(SLP, HIGH);

    loadSettings();
    motor.setCurrentPosition(loadPosition());
    targetPos = motor.currentPosition();
    motor.setMaxSpeed(jogSpeed);
    motor.setAcceleration(ACCELERATION);

    Serial.begin(9600);

    prevCmdTime = millis();
}

float gotoSpeed()
{
    switch (speedCode) {
        case 0x02: return 800;
        case 0x04: return 400;
        case 0x08: return 200;
        case 0x10: return 100;
        default:   return 50;
    }
}

void enable()
{
    if (standby) {
        digitalWrite(SLP, HIGH);
        delay(2);               // DRV8825 needs ~1.7 ms after wake before STEP
        standby = false;
    }
}

void disable()
{
    if (!standby) {
        digitalWrite(SLP, LOW);
        standby = true;
    }
}

void jog(int8_t dir)
{
    enable();
    jogging = true;
    motor.setMaxSpeed(jogSpeed);
    motor.moveTo(dir < 0 ? POS_MIN : POS_MAX);
}

void gotoTarget()
{
    enable();
    jogging = false;
    motor.setMaxSpeed(gotoSpeed());
    motor.moveTo(targetPos);
}

// Decelerate to rest. run() keeps being called until the motor stops, so
// AccelStepper's speed winds down to 0 instead of being left stale.
void halt()
{
    jogging = false;
    motor.stop();
}

void setJogSpeed(float speed)
{
    jogSpeed = speed;
    if (jogging) {
        motor.setMaxSpeed(jogSpeed);
    }
}

void reply(const char *s)
{
    Serial.print(s);
}

void replyHex(unsigned int value, uint8_t digits)
{
    char out[8];
    snprintf(out, sizeof(out), digits == 4 ? "%04X#" : "%02X#", value);
    Serial.print(out);
}

bool parseHex(const char *s, long &value)
{
    char *end;
    value = strtol(s, &end, 16);
    return end != s && *end == 0;
}

void moonliteCommand(const char *c)
{
    long v;
    if (strcmp(c, "GP") == 0) {
        replyHex(motor.currentPosition(), 4);
    } else if (strcmp(c, "GN") == 0) {
        replyHex(targetPos, 4);
    } else if (strcmp(c, "GI") == 0) {
        reply(motor.isRunning() ? "01#" : "00#");
    } else if (strcmp(c, "GV") == 0) {
        reply("10#");
    } else if (strcmp(c, "GD") == 0) {
        replyHex(speedCode, 2);
    } else if (strcmp(c, "GH") == 0) {
        reply("00#");
    } else if (strcmp(c, "GT") == 0) {
        reply("0000#");
    } else if (strcmp(c, "GC") == 0) {
        reply("00#");
    } else if (strcmp(c, "GK") == 0) {
        reply(keepAwake ? "01#" : "00#");
    } else if (strcmp(c, "FG") == 0) {
        gotoTarget();
    } else if (strcmp(c, "FQ") == 0) {
        halt();
    } else if (strncmp(c, "SP", 2) == 0 && parseHex(c + 2, v)) {
        motor.setCurrentPosition(constrain(v, POS_MIN, POS_MAX));
        targetPos = motor.currentPosition();
    } else if (strncmp(c, "SN", 2) == 0 && parseHex(c + 2, v)) {
        targetPos = constrain(v, POS_MIN, POS_MAX);
    } else if (strncmp(c, "SD", 2) == 0 && parseHex(c + 2, v)) {
        if (v == 0x02 || v == 0x04 || v == 0x08 || v == 0x10 || v == 0x20) {
            speedCode = v;
            if (motor.isRunning() && !jogging) {
                motor.setMaxSpeed(gotoSpeed());
            }
        }
    } else if (strncmp(c, "SK", 2) == 0 && parseHex(c + 2, v)) {
        if (keepAwake != (v != 0)) {
            keepAwake = v != 0;
            saveSettings();
        }
        if (keepAwake) {
            enable();
        }
    }
    // SH, SF, SC, PO, C, +, - and anything unknown: no reply
}

void legacyCommand(char c)
{
    switch (c) {
        case 'i': jog(-1); break;
        case 'o': jog(1); break;
        case 's': halt(); break;
        case 'h': setJogSpeed(JOG_HIGH_SPEED); break;
        case 'l': setJogSpeed(JOG_LOW_SPEED); break;
    }
}

void readSerial()
{
    while (Serial.available() > 0) {
        char c = Serial.read();
        if (c == ':') {
            inFrame = true;
            frameLen = 0;
        } else if (inFrame) {
            if (c == '#') {
                frame[frameLen] = 0;
                inFrame = false;
                moonliteCommand(frame);
            } else if (frameLen < sizeof(frame) - 1) {
                frame[frameLen++] = c;
            } else {
                inFrame = false;
            }
        } else {
            legacyCommand(c);
        }
    }
}

// -1 in, 1 out, 0 neither (or both)
int8_t readButtons()
{
    bool in = digitalRead(IN_BTN) == LOW;
    bool out = digitalRead(OUT_BTN) == LOW;
    return in == out ? 0 : (in ? -1 : 1);
}

void checkButtons()
{
    int8_t raw = readButtons();
    if (raw != btnRaw) {
        btnRaw = raw;
        btnChanged = millis();
    } else if (raw != btnState && millis() - btnChanged >= DEBOUNCE_MS) {
        btnState = raw;
        if (raw != 0) {
            jog(raw);
        } else if (jogging) {
            halt();
        }
    }
}

void loop()
{
    readSerial();
    checkButtons();

    if (motor.run()) {
        prevCmdTime = millis();
    } else {
        if (motor.currentPosition() != savedPos) {
            savePosition(motor.currentPosition());
        }
        if (!keepAwake && !standby && millis() - prevCmdTime > STANDBY_MS) {
            disable();
        }
    }
}
