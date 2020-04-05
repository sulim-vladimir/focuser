#include <AccelStepper.h>

#define M0  8
#define M1  7
#define M2  6
#define SLP 4
#define IN_BTN 3
#define OUT_BTN 2

AccelStepper motor(1, A2, A3);
unsigned long prevCmdTime;
bool standby = false;
bool move = false;
bool highSpeed = false;
char cmd = 0;

void setup() {
	pinMode(M0, OUTPUT); 
	pinMode(M1, OUTPUT); 
	pinMode(M2, OUTPUT); 
	pinMode(IN_BTN, INPUT); 
	pinMode(OUT_BTN, INPUT); 

	digitalWrite(M0, LOW);
	digitalWrite(M1, HIGH);
	digitalWrite(M2, HIGH);
	digitalWrite(IN_BTN, HIGH);
	digitalWrite(OUT_BTN, HIGH);

	pinMode(SLP, OUTPUT); 
	digitalWrite(SLP, HIGH);

	motor.setMaxSpeed(50);
	motor.setAcceleration(300);

	Serial.begin(9600);

	prevCmdTime = millis();
}

void moveIn()
{
	if (!move) {
		move = true;
		motor.moveTo(-1000000);
	}
}

void moveOut()
{
	if (!move) {
		move = true;
		motor.moveTo(1000000);
	}
}

void enable()
{
	if (standby) {
		digitalWrite(SLP, HIGH);
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

void stop()
{
	if (move) {
		move = false;
		motor.stop();
		prevCmdTime = millis();
	}
}

bool needStandby()
{
	return !standby && millis() - prevCmdTime > 5000;
}

bool cmdMoveIn()
{
	return cmd == 'i' || digitalRead(IN_BTN) == LOW;
}

bool cmdMoveOut()
{	
	return cmd == 'o' || digitalRead(OUT_BTN) == LOW;
}

bool cmdStop() 
{
	if (cmd != 0) {
		if (cmd == 's') {
			cmd = 0;
			return true;
		} else {
			return false;
		}
	}
	return digitalRead(IN_BTN) == HIGH && digitalRead(OUT_BTN) == HIGH; 
}

bool cmdHighSpeed()
{
	return cmd == 'h';
}

bool cmdLowSpeed()
{
    return cmd == 'l';
}

bool setHighSpeed()
{
    if (!highSpeed) {
        highSpeed = true;
        motor.setMaxSpeed(200);			
    }
}

bool setLowSpeed()
{
    if (highSpeed) {
        highSpeed = false;
        motor.setMaxSpeed(50);      
    }
}

void loop() 
{
	if (Serial.available() > 0) {
		cmd = Serial.read();
	}

    if (cmdHighSpeed()) {
        setHighSpeed();
    } else if (cmdLowSpeed()) {
    setLowSpeed();
	} else if (cmdMoveIn()) {
		enable();
		moveIn();
	} else if (cmdMoveOut()) {
		enable();
		moveOut();
	} else if (cmdStop()) {
		stop();
	}
	
	if (move) {
		motor.run();
	} else if (needStandby()) {
		disable();
	}
}
