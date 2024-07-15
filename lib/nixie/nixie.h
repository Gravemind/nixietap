#ifndef _NIXIE_h /* Include guard */
#define _NIXIE_h

#include <Arduino.h>
#include <TimeLib.h>
#include <SPI.h>
#include <BQ32000RTC.h>

#define RTC_SDA_PIN D3
#define RTC_SCL_PIN D4
#define RTC_IRQ_PIN D1
#define SPI_CS D8
#define TOUCH_BUTTON D2
#define CONFIG_BUTTON D0

#ifndef DEBUG
#define DEBUG
#endif // DEBUG

class Nixie {
	// Initialize the display. This function configures pinModes based on .h file.
	String oldNumber = "";
	uint8_t numberArray[100], numIsNeg;
	int dotPos, numberSize, k = 0;
	unsigned long previousMillis = 0;
	uint8_t autoPoisonDoneOnMinute = 0;
	uint8_t oldDigit1, oldDigit2, oldDigit3, oldDigit4;
	bool animate = false;

    public:
	Nixie();
	void begin();
	void write(uint8_t digit1, uint8_t digit2, uint8_t digit3, uint8_t digit4, uint8_t dots);
	void writeNumber(String newNumber, unsigned int movingSpeed);
	void writeTime(time_t local, bool dot_state, bool timeFormat);
	void writeDate(time_t local, bool dot_state);
	uint8_t checkDate(uint16_t y, uint8_t m, uint8_t d, uint8_t h, uint8_t mm);
	void antiPoison(time_t local, bool timeFormat);
	void setAnimation(bool animate);

    private:
	void writeLowLevel(uint8_t digit1, uint8_t digit2, uint8_t digit3, uint8_t digit4, uint8_t dots);
};
extern Nixie nixieTap;
#endif // _NIXIE_h
