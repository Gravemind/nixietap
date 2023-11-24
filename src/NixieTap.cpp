#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <AceTime.h>
#include <nixie.h>
#include <BQ32000RTC.h>
#include <NtpClientLib.h>
#include <TimeLib.h>
#include <EEPROM.h>
#include <Ticker.h>
#include <map>

using namespace ace_time;

IRAM_ATTR void irq_1Hz_int(); // Interrupt function for changing the dot state every 1 second.
IRAM_ATTR void touchButtonPressed(); // Interrupt function when button is pressed.
IRAM_ATTR void scrollDots(); // Interrupt function for scrolling dots.

void connectWiFi();
void disableSecDot();
void enableSecDot();
void firstRunInit();
void parseSerialSet(String);
void printTime(time_t);
void processSyncEvent(NTPSyncEvent_t ntpEvent);
void readAndParseSerial();
void readButton();
void readParameters();
void resetEepromToDefault();
void setSystemTimeFromRTC();
void startNTPClient();

volatile bool dot_state = LOW;
bool stopDef = false, secDotDef = false;
bool wifiFirstConnected = true;
bool syncEventTriggered = false; // True if a time event has been triggered.

uint8_t configButton = 0;
volatile uint8_t state = 0, dotPosition = 0b10;
char buttonCounter;
Ticker movingDot; // Initializing software timer interrupt called movingDot.
NTPSyncEvent_t ntpEvent; // Last triggered event.
time_t t;
time_t last_wifi_connect_attempt;
String serialCommand = "";
ESP8266WiFiMulti wifiMulti;

uint8_t timeRefreshFlag;

const char *NixieTap = "NixieTap";

char cfg_target_SSID[50] = "\0";
char cfg_target_pw[50] = "\0";
char cfg_ntp_server[50] = "time.google.com";
char cfg_time_zone[50] = "America/New_York";
uint32_t cfg_ntp_sync_interval = 3671;
uint8_t cfg_manual_time_flag = 1;
uint8_t cfg_enable_24h = 1;

std::map<String, int> mem_map;

static const int TZ_CACHE_SIZE = 1;
static ExtendedZoneProcessorCache<TZ_CACHE_SIZE> zoneProcessorCache;
static ExtendedZoneManager zoneManager(
	zonedbx::kZoneAndLinkRegistrySize,
	zonedbx::kZoneAndLinkRegistry,
	zoneProcessorCache);
TimeZone time_zone;

void setup()
{
	mem_map["target_ssid"] = 100;
	mem_map["target_pw"] = 150;
	mem_map["ntp_server"] = 200;
	mem_map["time_zone"] = 250;
	mem_map["manual_time_flag"] = 381;
	mem_map["enable_24h"] = 387;
	mem_map["ntp_sync_interval"] = 388;
	mem_map["non_init"] = 500;

	Serial.println("\r\n\r\n\r\nNixie Tap is booting!");

	// Set WiFi station mode settings.
	WiFi.mode(WIFI_STA);
	WiFi.hostname(NixieTap);
	wifiMulti.addAP(cfg_target_SSID, cfg_target_pw);

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

	// Load time zone.
	time_zone = zoneManager.createForZoneName(cfg_time_zone);
	if (time_zone.isError()) {
		Serial.println("Unable to load time zone, using UTC.");

		// Use UTC instead.
		time_zone = zoneManager.createForZoneInfo(&zonedbx::kZoneEtc_UTC);
		if (time_zone.isError()) {
			Serial.println("WARNING! Unable to load UTC time zone.");
		}
	}

	// Progress bar: 75%.
	nixieTap.write(10, 10, 10, 10, 0b1110);

	// Set the system time from the on-board RTC.
	setSystemTimeFromRTC();
	printTime(now());

	enableSecDot();

	connectWiFi();

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

	// Check if we should attempt to reconnect to WiFi.
	if (WiFi.status() != WL_CONNECTED && t - last_wifi_connect_attempt > 30) {
		connectWiFi();
	}

	// If time is configured to be set semi-auto or auto and NixieTap is just started, the NTP client is started.
	if (cfg_manual_time_flag == 0 && wifiFirstConnected && WiFi.status() == WL_CONNECTED) {
		startNTPClient();
		wifiFirstConnected = false;
	}
	if (syncEventTriggered) {
		processSyncEvent(ntpEvent);
		syncEventTriggered = false;
	}

	// Calculate the offset from UTC at the current instant.
	int32_t offset = ZonedDateTime::forUnixSeconds64(t, time_zone).timeOffset().toSeconds();

	// State machine
	if (state > 1)
		state = 0;

	// Slot 0 - time
	if (state == 0) {
		nixieTap.writeTime(t + offset, dot_state, cfg_enable_24h);
	}

	// Slot 1 - date
	if (state == 1) {
		nixieTap.writeDate(t + offset, 1);
	}
}

void connectWiFi()
{
	if (cfg_target_SSID[0] != '\0' && cfg_target_pw[0] != '\0') {
		Serial.print("Connecting to Wi-Fi access point: ");
		Serial.println(cfg_target_SSID);

		if (wifiMulti.run(5000 /* connect timeout */) == WL_CONNECTED) {
			Serial.print("Connected, IP address: ");
			Serial.println(WiFi.localIP());
		}
	}

	last_wifi_connect_attempt = now();
}

void setSystemTimeFromRTC()
{
	time_t rtc_time = RTC.get();
	setTime(rtc_time);
	Serial.println("System time has been set from the on-board RTC.");
}

void startNTPClient()
{
	Serial.println("Starting NTP client.");

	NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {
		ntpEvent = event;
		syncEventTriggered = true;
	});

	if (!NTP.setInterval(cfg_ntp_sync_interval)) {
		Serial.println("Failed to set NTP sync interval!");
	}

	if (!NTP.begin(cfg_ntp_server)) {
		Serial.println("Failed to start NTP client!");
	}
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

	int EEaddress = mem_map["target_ssid"];
	EEPROM.get(EEaddress, cfg_target_SSID);
	Serial.print("[EEPROM Read] ");
	Serial.println("target_ssid: " + (String)cfg_target_SSID);

	EEaddress = mem_map["target_pw"];
	EEPROM.get(EEaddress, cfg_target_pw);
	Serial.print("[EEPROM Read] ");
	Serial.println("target_pw: " + (String)cfg_target_pw);

	EEaddress = mem_map["ntp_server"];
	EEPROM.get(EEaddress, cfg_ntp_server);
	Serial.print("[EEPROM Read] ");
	Serial.println("ntp_server: " + (String)cfg_ntp_server);

	EEaddress = mem_map["time_zone"];
	EEPROM.get(EEaddress, cfg_time_zone);
	Serial.print("[EEPROM Read] ");
	Serial.println("time_zone: " + (String)cfg_time_zone);

	EEaddress = mem_map["manual_time_flag"];
	EEPROM.get(EEaddress, cfg_manual_time_flag);
	Serial.print("[EEPROM Read] ");
	Serial.println("manual_time_flag: " + (String)cfg_manual_time_flag);

	EEaddress = mem_map["enable_24h"];
	EEPROM.get(EEaddress, cfg_enable_24h);
	Serial.print("[EEPROM Read] ");
	Serial.println("enable_24h: " + (String)cfg_enable_24h);

	EEaddress = mem_map["ntp_sync_interval"];
	EEPROM.get(EEaddress, cfg_ntp_sync_interval);
	Serial.print("[EEPROM Read] ");
	Serial.println("ntp_sync_interval: " + (String)cfg_ntp_sync_interval);
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
			} else if (serialCommand == "set") {
				Serial.println("Available 'set' commands: time.");
			} else if (serialCommand.startsWith("set ")) {
				parseSerialSet(serialCommand.substring(strlen("set ")));
			} else if (serialCommand == "time") {
				printTime(now());
			} else if (serialCommand == "help") {
				Serial.println("Available commands: init, read, restart, set, time, help.");
			} else {
				Serial.print("Unknown command: ");
				Serial.println(serialCommand);
			}

			serialCommand = "";
		}
	}
}

void parseSerialSet(String s)
{
	if (s.startsWith("time ")) {
		String s_time = s.substring(strlen("time "));
		auto odt = OffsetDateTime::forDateString(s_time.c_str());
		if (!odt.isError()) {
			time_t odt_unix = odt.toUnixSeconds64();
			setTime(odt_unix);
			RTC.set(odt_unix);
			printTime(odt_unix);
		} else {
			Serial.print("Unable to parse timestamp: ");
			Serial.println(s_time);
		}
	} else {
		Serial.print("Unable to parse 'set' command: ");
		Serial.println(s);
	}
}

void printTime(time_t t)
{
	Serial.print("The time is now: ");
	ZonedDateTime::forUnixSeconds64(t, time_zone).printTo(Serial);
	Serial.print(" @ ");
	Serial.println(t);
}

void resetEepromToDefault()
{
	Serial.println("Writing factory defaults to EEPROM...");

	EEPROM.begin(512);

	int EEaddress = mem_map["target_ssid"];
	EEPROM.put(EEaddress, "");
	Serial.print("[EEPROM Reset] ");
	Serial.println("Clearing station mode SSID network name");

	EEaddress = mem_map["target_pw"];
	EEPROM.put(EEaddress, "");
	Serial.print("[EEPROM Reset] ");
	Serial.println("Clearing station mode SSID network password");

	EEaddress = mem_map["ntp_server"];
	EEPROM.put(EEaddress, "time.google.com");
	Serial.print("[EEPROM Reset] ");
	Serial.println("ntp_server: time.google.com");

	EEaddress = mem_map["time_zone"];
	EEPROM.put(EEaddress, "America/New_York");
	Serial.print("[EEPROM Reset] ");
	Serial.println("time_zone: America/New_York");

	EEaddress = mem_map["manual_time_flag"];
	EEPROM.put(EEaddress, 1);
	Serial.print("[EEPROM Reset] ");
	Serial.println("manual_time_flag: 1");

	EEaddress = mem_map["enable_24h"];
	EEPROM.put(EEaddress, 1);
	Serial.print("[EEPROM Reset] ");
	Serial.println("enable_24h: 1");

	EEaddress = mem_map["ntp_sync_interval"];
	EEPROM.put(EEaddress, 1);
	Serial.print("[EEPROM Reset] ");
	Serial.println("ntp_sync_interval: 3671");

	EEPROM.commit();
}

void readButton()
{
	configButton = digitalRead(CONFIG_BUTTON);
	if (configButton) {
		Serial.println("Button pressed.");
		buttonCounter++;
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
