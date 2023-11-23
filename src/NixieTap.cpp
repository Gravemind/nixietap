#include <Arduino.h>
#include <nixie.h>
#include <BQ32000RTC.h>
#include <NtpClientLib.h>
#include <TimeLib.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Ticker.h>
#include <map>

IRAM_ATTR void irq_1Hz_int(); // Interrupt function for changing the dot state every 1 second.
IRAM_ATTR void touchButtonPressed(); // Interrupt function when button is pressed.
IRAM_ATTR void scrollDots(); // Interrupt function for scrolling dots.

void disableSecDot();
void enableSecDot();
void firstRunInit();
void printTime(time_t);
void processSyncEvent(NTPSyncEvent_t ntpEvent);
void readAndParseSerial();
void readButton();
void readParameters();
void resetEepromToDefault();
void startPortalManually();
void updateParameters();
void updateTime();

volatile bool dot_state = LOW;
bool stopDef = false, secDotDef = false;
bool wifiFirstConnected = true;
bool syncEventTriggered = false; // True if a time event has been triggered.

uint8_t configButton = 0;
volatile uint8_t state = 0, dotPosition = 0b10;
char buttonCounter;
Ticker movingDot; // Initializing software timer interrupt called movingDot.
NTPSyncEvent_t ntpEvent; // Last triggered event.
WiFiManager wifiManager;
time_t t;
String serialCommand = "";

uint8_t timeRefreshFlag;

const char *NixieTap = "NixieTap";

char cfg_time[6] = "00:00";
char cfg_date[11] = "1970-01-01";
char cfg_SSID[50] = "NixieTap";
char cfg_password[50] = "nixietap";
char cfg_target_SSID[50] = "\0";
char cfg_target_pw[50] = "\0";
uint8_t cfg_enable_time = 1;
uint8_t cfg_enable_date = 1;
uint8_t cfg_manual_time_flag = 1;
uint8_t cfg_enable_24h = 1;

std::map<String, int> mem_map;

void setup()
{
	mem_map["SSID"] = 0;
	mem_map["password"] = 50;
	mem_map["target_ssid"] = 100;
	mem_map["target_pw"] = 150;
	mem_map["manual_time_flag"] = 381;
	mem_map["enable_date"] = 382;
	mem_map["enable_time"] = 383;
	mem_map["enable_24h"] = 387;
	mem_map["non_init"] = 500;

	Serial.println("");
	Serial.println("");
	Serial.println("");
	Serial.println(" Nixie Tap is booting!");

	// Set WiFi station mode settings.
	WiFi.mode(WIFI_STA);
	WiFi.hostname(NixieTap);

	// Progress bar: 25%.
	nixieTap.write(10, 10, 10, 10, 0b10);

	// Touch button interrupt.
	attachInterrupt(digitalPinToInterrupt(TOUCH_BUTTON), touchButtonPressed, RISING);

	// Progress bar: 50%.
	nixieTap.write(10, 10, 10, 10, 0b110);

	// Reset EEPROM if uninitialized.
	firstRunInit();

	// Read all stored parameters from EEPROM.
	readParameters();

	// Progress bar: 75%.
	nixieTap.write(10, 10, 10, 10, 0b1110);

	// Configure the Time library to obtain the time from the on-board RTC.
	setSyncProvider(RTC.get);
	switch (timeStatus()) {
	case timeSet:
		Serial.println("System time has been set and synchronized with the on-board RTC.");
		break;
	case timeNotSet:
		Serial.println("System time has not been set.");
		break;
	case timeNeedsSync:
		Serial.println("System time has been set but a sync attempt did not succeed.");
		break;
	default:
		Serial.println("Unknown time synchronization error!");
	}
	printTime(now());

	enableSecDot();

	// Connect to WiFi.
	if (cfg_target_SSID[0] != '\0' && cfg_target_pw[0] != '\0') {
		Serial.print("Connecting to Wi-Fi access point: ");
		Serial.println(cfg_target_SSID);
		wifiManager.connectWifi(cfg_target_SSID, cfg_target_pw);
	}

	// Progress bar: 100%.
	nixieTap.write(10, 10, 10, 10, 0b11110);
}

void loop()
{
	// Polling functions
	readAndParseSerial();
	readButton();

	// Mandatory functions to be executed every cycle
	t = now(); // update date and time variable

	// If time is configured to be set semi-auto or auto and NixieTap is just started, the NTP client is started.
	if (cfg_manual_time_flag == 0 && wifiFirstConnected && WiFi.status() == WL_CONNECTED) {
		NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {
			ntpEvent = event;
			syncEventTriggered = true;
		});
		NTP.setInterval(63); /* XXX: Remove this after testing */
		Serial.println("Starting NTP client.");
		NTP.begin();
		wifiFirstConnected = false;
	}
	if (syncEventTriggered) {
		processSyncEvent(ntpEvent);
		syncEventTriggered = false;
	}

	// State machine
	if (state > 1)
		state = 0;

	// Slot 0 - time
	if (state == 0 && cfg_enable_time) {
		nixieTap.writeTime(t, dot_state, cfg_enable_24h);
	} else if (!cfg_enable_time && state == 0)
		state++;

	// Slot 1 - date
	if (state == 1 && cfg_enable_date) {
		nixieTap.writeDate(t, 1);
	} else if (!cfg_enable_date && state == 1)
		state++;

	// Here you can add new functions for displaying numbers on NixieTap, just follow the basic writing principle from above.
}

void startPortalManually()
{
	// By pressing the button on the back of the device you can manually start the WiFi Manager and access it's settings.
	nixieTap.write(10, 10, 10, 10, 0);
	disableSecDot(); // If the dots are not disabled, precisely the RTC_IRQ_PIN interrupt, ConfigPortal will chrach.
	movingDot.attach(0.2, scrollDots);
	wifiManager.setConfigPortalTimeout(1800);
	// This will run a new config portal if the SSID and PW are valid.
	if (!wifiManager.startConfigPortal(cfg_SSID, cfg_password)) {
		Serial.println("Failed to connect and hit timeout!");
		// If the NixieTap is not connected to WiFi, it will collect the entered parameters and configure the RTC according to them.
	}
	updateParameters();
	updateTime();
	movingDot.detach();
	nixieTap.write(10, 10, 10, 10, 0); // Deletes remaining dot on display.
	enableSecDot();
}

void processSyncEvent(NTPSyncEvent_t ntpEvent)
{
	// When syncEventTriggered is triggered, through NTPClient, Nixie checks if NTP time is received.
	// If NTP time is received, Nixie starts synchronization of RTC time with received NTP time.

	if (ntpEvent < 0) {
		Serial.print("Time sync error: ");
		if (ntpEvent == noResponse) {
			Serial.println("NTP server not reachable.");
		} else if (ntpEvent == invalidAddress) {
			Serial.println("Invalid NTP server address.");
		} else if (ntpEvent == errorSending) {
			Serial.println("Error sending request");
		} else if (ntpEvent == responseError) {
			Serial.println("NTP response error");
		}
	} else {
		if (ntpEvent == timeSyncd && NTP.SyncStatus()) {
			time_t ntp_time = NTP.getLastNTPSync();
			RTC.set(ntp_time);
			printTime(ntp_time);
		}
	}
}

void readParameters()
{
	Serial.println("Reading saved parameters from EEPROM.");

	int EEaddress = mem_map["SSID"];
	EEPROM.get(EEaddress, cfg_SSID);
	Serial.print("[EEPROM Read] ");
	Serial.println("SSID: " + (String)cfg_SSID);

	EEaddress = mem_map["password"];
	EEPROM.get(EEaddress, cfg_password);
	Serial.print("[EEPROM Read] ");
	Serial.println("password: " + (String)cfg_password);

	EEaddress = mem_map["target_ssid"];
	EEPROM.get(EEaddress, cfg_target_SSID);
	Serial.print("[EEPROM Read] ");
	Serial.println("target_ssid: " + (String)cfg_target_SSID);

	EEaddress = mem_map["target_pw"];
	EEPROM.get(EEaddress, cfg_target_pw);
	Serial.print("[EEPROM Read] ");
	Serial.println("target_pw: " + (String)cfg_target_pw);

	EEaddress = mem_map["manual_time_flag"];
	EEPROM.get(EEaddress, cfg_manual_time_flag);
	Serial.print("[EEPROM Read] ");
	Serial.println("manual_time_flag: " + (String)cfg_manual_time_flag);

	EEaddress = mem_map["enable_date"];
	EEPROM.get(EEaddress, cfg_enable_date);
	Serial.print("[EEPROM Read] ");
	Serial.println("enable_date: " + (String)cfg_enable_date);

	EEaddress = mem_map["enable_time"];
	EEPROM.get(EEaddress, cfg_enable_time);
	Serial.print("[EEPROM Read] ");
	Serial.println("enable_time: " + (String)cfg_enable_time);

	EEaddress = mem_map["enable_24h"];
	EEPROM.get(EEaddress, cfg_enable_24h);
	Serial.print("[EEPROM Read] ");
	Serial.println("enable_24h: " + (String)cfg_enable_24h);
}

void updateParameters()
{
	Serial.println("Synchronization of parameters started.");
	EEPROM.begin(512); // Number of bytes to allocate for parameters.
	int EEaddress;
	Serial.println("Comparing entered keys with the saved ones.");
	if (wifiManager.nixie_params.count("SSID") == 1) {
		const char *nixie_ssid = wifiManager.nixie_params["SSID"].c_str();
		if (nixie_ssid[0] != '\0' and cfg_SSID != nixie_ssid) {
			EEaddress = mem_map["SSID"];
			strcpy(cfg_SSID, nixie_ssid);
			EEPROM.put(EEaddress, cfg_SSID);
			const char *nixie_pw = wifiManager.nixie_params["hotspot_password"].c_str();
			if (nixie_pw[0] != '\0' and cfg_password != nixie_pw) {
				EEaddress = mem_map["password"];
				strcpy(cfg_password, nixie_pw);
				EEPROM.put(EEaddress, cfg_password);
			}
		}
	}
	if (wifiManager.nixie_params.count("target_ssid") == 1) {
		const char *new_target_ssid = wifiManager.nixie_params["target_ssid"].c_str();
		if (new_target_ssid[0] != '\0' and cfg_target_SSID != new_target_ssid) {
			EEaddress = mem_map["target_ssid"];
			strcpy(cfg_target_SSID, new_target_ssid);
			EEPROM.put(EEaddress, cfg_target_SSID);
			const char *new_target_pw = wifiManager.nixie_params["target_password"].c_str();
			if (new_target_pw[0] != '\0' and new_target_pw != cfg_target_pw) {
				EEaddress = mem_map["target_pw"];
				strcpy(cfg_target_pw, new_target_pw);
				EEPROM.put(EEaddress, cfg_target_pw);
				wifiManager.connectWifi(cfg_target_SSID, cfg_target_pw);
			}
		}
	}
	uint8_t new_enable_date = (uint8_t)wifiManager.nixie_params.count("enableDate");
	if (new_enable_date != cfg_enable_date) {
		EEaddress = mem_map["enable_date"];
		cfg_enable_date = new_enable_date;
		EEPROM.put(EEaddress, cfg_enable_date);
	}
	uint8_t new_enable_time = (uint8_t)wifiManager.nixie_params.count("enableTime");
	if (new_enable_time != cfg_enable_time) {
		EEaddress = mem_map["enable_time"];
		cfg_enable_time = new_enable_time;
		EEPROM.put(EEaddress, new_enable_time);
	}
	uint8_t new_enable_24h = (uint8_t)wifiManager.nixie_params.count("enable24h");
	if (cfg_enable_24h != new_enable_24h) {
		EEaddress = mem_map["enable_24h"];
		cfg_enable_24h = new_enable_24h;
		EEPROM.put(EEaddress, cfg_enable_24h);
	}
	if (wifiManager.nixie_params.count("setTimeManuallyFlag") == 1) {
		uint8_t new_manual_time_flag = atoi(wifiManager.nixie_params["setTimeManuallyFlag"].c_str());
		if (new_manual_time_flag != cfg_manual_time_flag) {
			EEaddress = mem_map["manual_time_flag"];
			uint8_t temp_time_flag = new_manual_time_flag;
			EEPROM.put(EEaddress, temp_time_flag);
		}
		cfg_manual_time_flag = new_manual_time_flag;
		timeRefreshFlag = 1;
	}
	if (wifiManager.nixie_params.count("time") == 1) {
		const char *new_time = wifiManager.nixie_params["time"].c_str();
		if (new_time[0] != '\0' and new_time != cfg_time) {
			strcpy(cfg_time, new_time);
			timeRefreshFlag = 1;
		}
	}
	if (wifiManager.nixie_params.count("date") == 1) {
		const char *new_date = wifiManager.nixie_params["date"].c_str();
		if (new_date[0] != '\0' and new_date != cfg_date) {
			strcpy(cfg_date, new_date);
			timeRefreshFlag = 1;
		}
	}
	// Setting the "non initialized" flag to 0
	EEaddress = mem_map["non_init"];
	EEPROM.put(EEaddress, 0);

	EEPROM.commit();
	wifiManager.nixie_params.clear();
	Serial.println("Synchronization of parameters completed!");
}

void updateTime()
{
	if (timeRefreshFlag) {
		if (cfg_manual_time_flag) { // I need feedback from the WiFiManager API that this option has been selected.
			NTP.stop(); // NTP sync is disableded to avoid sync errors.
			int hours = -1;
			int minutes = -1;
			char *time_token = strtok(cfg_time, ":");
			while (time_token != NULL) {
				if (hours == -1) {
					hours = atoi(time_token);
				} else {
					minutes = atoi(time_token);
				}
				time_token = strtok(NULL, " ");
			}
			int year = -1;
			int month = -1;
			int day = -1;
			char *date_token = strtok(cfg_date, "-");
			while (date_token != NULL) {
				if (year == -1) {
					year = atoi(date_token);
				} else if (month == -1) {
					month = atoi(date_token);
				} else {
					day = atoi(date_token);
				}
				date_token = strtok(NULL, "-");
			}
			setTime(hours, minutes, 0, day, month, year);
			t = now();
			RTC.set(t);
			setSyncProvider(RTC.get);
			Serial.println("Manually entered date and time saved!");
		} else if (WiFi.status() == WL_CONNECTED) {
			Serial.println("NixieTap is auto and connected, setting time to NTP!");
			NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {
				ntpEvent = event;
				syncEventTriggered = true;
			});
			Serial.println("Starting NTP client.");
			NTP.begin();
			wifiFirstConnected = false;
		} else {
			Serial.println("NixieTap not connected to WiFi, cannot auto sync time via NTP!");
		}
		timeRefreshFlag = 0;
	}
}

/*                                                           *
 *  Enables the center dot to change its state every second. *
 *                                                           */
void enableSecDot()
{
	if (secDotDef == false) {
		detachInterrupt(RTC_IRQ_PIN);
		RTC.setIRQ(1); // Configures the 512Hz interrupt from RTC.
		attachInterrupt(digitalPinToInterrupt(RTC_IRQ_PIN), irq_1Hz_int, FALLING);
		secDotDef = true;
		stopDef = false;
	}
}

/*                                                *
 * Disaling the dots function on nixie display.   *
 *                                                */
void disableSecDot()
{
	if (stopDef == false) {
		detachInterrupt(RTC_IRQ_PIN);
		RTC.setIRQ(0); // Configures the interrupt from RTC.
		dotPosition = 0b10; // Restast dot position.
		stopDef = true;
		secDotDef = false;
	}
}

/*                                                                                       *
 * An interrupt function that changes the state and position of the dots on the display. *
 *                                                                                       */
void scrollDots()
{
	if (dotPosition == 0b100000)
		dotPosition = 0b10;
	nixieTap.write(11, 11, 11, 11, dotPosition);
	dotPosition = dotPosition << 1;
}

/*                                                                  *
 * An interrupt function for changing the dot state every 1 second. *
 *                                                                  */
void irq_1Hz_int()
{
	dot_state = !dot_state;
}

/*                                                                *
 * An interrupt function for the touch sensor when it is touched. *
 *                                                                */
void touchButtonPressed()
{
	state++;
	nixieTap.setAnimation(true);
}

void readAndParseSerial()
{
	if (Serial.available() > 0) {
		serialCommand.concat(Serial.readStringUntil('\n'));

		if (serialCommand.endsWith("\r")) {
			serialCommand.trim();

			if (serialCommand == "init") {
				resetEepromToDefault();
			} else if (serialCommand == "read") {
				readParameters();
			} else if (serialCommand == "restart") {
				ESP.restart();
			} else if (serialCommand == "time") {
				printTime(now());
			} else if (serialCommand == "help") {
				Serial.println("Available commands: init, read, restart, time, help.");
			} else {
				Serial.println("Unknown command.");
			}

			serialCommand = "";
		}
	}
}

void printTime(time_t t)
{
	tmElements_t tm;
	breakTime(t, tm);

	Serial.print("The time is now: ");

	Serial.print(tmYearToCalendar(tm.Year));

	Serial.print('-');

	if (tm.Month < 10) {
		Serial.print('0');
	}
	Serial.print(tm.Month);

	Serial.print('-');
	if (tm.Day < 10) {
		Serial.print('0');
	}
	Serial.print(tm.Day);

	Serial.print('T');

	if (tm.Hour < 10) {
		Serial.print('0');
	}
	Serial.print(tm.Hour);

	Serial.print(':');

	if (tm.Minute < 10) {
		Serial.print('0');
	}
	Serial.print(tm.Minute);

	Serial.print(':');

	if (tm.Second < 10) {
		Serial.print('0');
	}
	Serial.print(tm.Second);

	Serial.print(" @ ");
	Serial.println(t);
}

void resetEepromToDefault()
{
	Serial.println("Writing factory defaults to EEPROM...");

	EEPROM.begin(512);

	int EEaddress = mem_map["SSID"];
	EEPROM.put(EEaddress, "NixieTap");
	Serial.print("[EEPROM Reset] ");
	Serial.println("AP mode SSID network name: NixieTap");

	EEaddress = mem_map["password"];
	EEPROM.put(EEaddress, "NixieTap");
	Serial.print("[EEPROM Reset] ");
	Serial.println("AP mode SSID network password: NixieTap");

	EEaddress = mem_map["target_ssid"];
	EEPROM.put(EEaddress, "");
	Serial.print("[EEPROM Reset] ");
	Serial.println("Clearing station mode SSID network name");

	EEaddress = mem_map["target_pw"];
	EEPROM.put(EEaddress, "");
	Serial.print("[EEPROM Reset] ");
	Serial.println("Clearing station mode SSID network password");

	EEaddress = mem_map["manual_time_flag"];
	EEPROM.put(EEaddress, 1);
	Serial.print("[EEPROM Reset] ");
	Serial.println("manual_time_flag: 1");

	EEaddress = mem_map["enable_date"];
	EEPROM.put(EEaddress, 1);
	Serial.print("[EEPROM Reset] ");
	Serial.println("enable_date: 1");

	EEaddress = mem_map["enable_time"];
	EEPROM.put(EEaddress, 1);
	Serial.print("[EEPROM Reset] ");
	Serial.println("enable_time: 1");

	EEaddress = mem_map["enable_24h"];
	EEPROM.put(EEaddress, 1);
	Serial.print("[EEPROM Reset] ");
	Serial.println("enable_24h: 1");

	EEPROM.commit();
}

void readButton()
{
	configButton = digitalRead(CONFIG_BUTTON);
	if (configButton) {
		buttonCounter++;
		if (buttonCounter == 5) {
			buttonCounter = 0;
			startPortalManually();
		}
	}
}

void firstRunInit()
{
	bool notInitialized = 1;
	EEPROM.begin(512);
	EEPROM.get(mem_map["non_init"], notInitialized);
	if (notInitialized) {
		Serial.println("Performing first run initialization...");
		resetEepromToDefault();
	}
}
