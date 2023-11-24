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

time_t t;
time_t last_wifi_connect_attempt;

uint8_t configButton = 0;
volatile uint8_t state = 0, dotPosition = 0b10;
uint32_t buttonCounter;
ESP8266WiFiMulti wifiMulti;
Ticker movingDot; // Initializing software timer interrupt called movingDot.
NTPSyncEvent_t ntpEvent; // Last triggered event.
String serialCommand = "";

const char *NixieTap = "NixieTap";

char cfg_ssid[50] = "\0";
char cfg_password[50] = "\0";
char cfg_ntp_server[50] = "\0";
char cfg_time_zone[50] = "\0";
uint8_t cfg_24hr_enabled = 1;
uint8_t cfg_ntp_enabled = 1;
uint32_t cfg_ntp_sync_interval = 3671;

#define DEFAULT__24HR_ENABLED		1
#define DEFAULT__NTP_ENABLED		1
#define DEFAULT__NTP_SERVER		"time.google.com"
#define DEFAULT__NTP_SYNC_INTERVAL	3671
#define DEFAULT__TIME_ZONE		"America/New_York"

#define EEPROM_ADDR__24HR_ENABLED	10	// 1 byte
#define EEPROM_ADDR__NTP_ENABLED	11	// 1 byte
#define EEPROM_ADDR__NTP_SYNC_INTERVAL	50	// 4 bytes
#define EEPROM_ADDR__SSID		100	// 50 bytes
#define EEPROM_ADDR__PASSWORD		150	// 50 bytes
#define EEPROM_ADDR__NTP_SERVER		200	// 50 bytes
#define EEPROM_ADDR__TIME_ZONE		250	// 50 bytes
#define EEPROM_ADDR__MAGIC		500	// 8 bytes

#define EEPROM_MAGIC			0x4e49584945544150

static const int TZ_CACHE_SIZE = 1;
static ExtendedZoneProcessorCache<TZ_CACHE_SIZE> zoneProcessorCache;
static ExtendedZoneManager zoneManager(
	zonedbx::kZoneAndLinkRegistrySize,
	zonedbx::kZoneAndLinkRegistry,
	zoneProcessorCache);
TimeZone time_zone;

void setup()
{
	Serial.println("\r\n\r\n\r\nNixie Tap is booting!");

	// Set WiFi station mode settings.
	WiFi.mode(WIFI_STA);
	WiFi.hostname(NixieTap);
	wifiMulti.addAP(cfg_ssid, cfg_password);

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

	// Start the NTP client if enabled and connected to WiFi.
	if (cfg_ntp_enabled == 1 && wifiFirstConnected && WiFi.status() == WL_CONNECTED) {
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
		nixieTap.writeTime(t + offset, dot_state, cfg_24hr_enabled);
	}

	// Slot 1 - date
	if (state == 1) {
		nixieTap.writeDate(t + offset, 1);
	}
}

void connectWiFi()
{
	if (cfg_ssid[0] != '\0' && cfg_password[0] != '\0') {
		Serial.print("Connecting to Wi-Fi access point: ");
		Serial.println(cfg_ssid);

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

void readParameters()
{
	Serial.println("Reading saved parameters from EEPROM...");

	EEPROM.get(EEPROM_ADDR__24HR_ENABLED, cfg_24hr_enabled);
	Serial.print("[EEPROM Read] ");
	Serial.print("24hr_enabled: ");
	Serial.println(cfg_24hr_enabled);

	EEPROM.get(EEPROM_ADDR__NTP_ENABLED, cfg_ntp_enabled);
	Serial.print("[EEPROM Read] ");
	Serial.print("ntp_enabled: ");
	Serial.println(cfg_ntp_enabled);

	EEPROM.get(EEPROM_ADDR__NTP_SYNC_INTERVAL, cfg_ntp_sync_interval);
	Serial.print("[EEPROM Read] ");
	Serial.print("ntp_sync_interval: ");
	Serial.println(cfg_ntp_sync_interval);

	EEPROM.get(EEPROM_ADDR__NTP_SERVER, cfg_ntp_server);
	Serial.print("[EEPROM Read] ");
	Serial.print("ntp_server: ");
	Serial.println(cfg_ntp_server);

	EEPROM.get(EEPROM_ADDR__TIME_ZONE, cfg_time_zone);
	Serial.print("[EEPROM Read] ");
	Serial.print("time_zone: ");
	Serial.println(cfg_time_zone);

	EEPROM.get(EEPROM_ADDR__SSID, cfg_ssid);
	Serial.print("[EEPROM Read] ");
	Serial.print("ssid: ");
	Serial.println(cfg_ssid);

	EEPROM.get(EEPROM_ADDR__PASSWORD, cfg_password);
	Serial.print("[EEPROM Read] ");
	Serial.print("password: ");
	Serial.println(cfg_password);
}

void resetEepromToDefault()
{
	Serial.println("Writing factory defaults to EEPROM...");

	EEPROM.begin(512);

	EEPROM.put(EEPROM_ADDR__24HR_ENABLED, DEFAULT__24HR_ENABLED);
	Serial.print("[EEPROM Reset] ");
	Serial.print("24hr_enabled: ");
	Serial.println(DEFAULT__24HR_ENABLED);

	EEPROM.put(EEPROM_ADDR__NTP_ENABLED, DEFAULT__NTP_ENABLED);
	Serial.print("[EEPROM Reset] ");
	Serial.print("ntp_enabled: ");
	Serial.println(DEFAULT__NTP_ENABLED);

	EEPROM.put(EEPROM_ADDR__NTP_SERVER, DEFAULT__NTP_SERVER);
	Serial.print("[EEPROM Reset] ");
	Serial.print("ntp_server: ");
	Serial.println(DEFAULT__NTP_ENABLED);

	EEPROM.put(EEPROM_ADDR__NTP_SYNC_INTERVAL, DEFAULT__NTP_SYNC_INTERVAL);
	Serial.print("[EEPROM Reset] ");
	Serial.print("ntp_sync_interval: ");
	Serial.println(DEFAULT__NTP_SYNC_INTERVAL);

	EEPROM.put(EEPROM_ADDR__TIME_ZONE, DEFAULT__TIME_ZONE);
	Serial.print("[EEPROM Reset] ");
	Serial.print("time_zone: ");
	Serial.println(DEFAULT__TIME_ZONE);

	EEPROM.put(EEPROM_ADDR__SSID, "");
	Serial.print("[EEPROM Reset] ");
	Serial.println("ssid: (not set)");

	EEPROM.put(EEPROM_ADDR__PASSWORD, "");
	Serial.print("[EEPROM Reset] ");
	Serial.println("password: (not set)");

	EEPROM.put(EEPROM_ADDR__MAGIC, EEPROM_MAGIC);

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
	uint64_t magic = 0;
	EEPROM.begin(512);
	EEPROM.get(EEPROM_ADDR__MAGIC, magic);
	if (magic != EEPROM_MAGIC) {
		Serial.println("EEPROM magic value mismatch.");
		resetEepromToDefault();
	}
}
