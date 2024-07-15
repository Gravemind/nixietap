#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <AceTime.h>
#include <nixie.h>
#include <BQ32000RTC.h>
#include <NtpClientLib.h>
#include <TimeLib.h>
#include <EEPROM.h>

using namespace ace_time;

// Interrupt function for changing the dot state every 1 second.
IRAM_ATTR void irq_1Hz_int();

// Interrupt function when button is pressed.
IRAM_ATTR void touchButtonPressed();

const char *wifiDisconnectReasonStr(const enum WiFiDisconnectReason);
void connectWiFi();
void enableSecDot();
void firstRunInit();
void loadTimeZone();
void parseSerialSet(String);
void printESPInfo();
void printTime(time_t);
void processSyncEvent(NTPSyncEvent_t);
void readAndParseSerial();
void readConfigButton();
void readParameters();
void resetEepromToDefault();
void setSystemTimeFromRTC();
void setupWiFi();
void startNTPClient();
void stopNTPClient();

volatile bool dot_state = LOW;
volatile bool touch_button_pressed = false;
bool stopDef = false, secDotDef = false;
bool serialTicker = false;
bool ntpInitialized = false;
bool syncEventTriggered = false;

time_t current_time;
time_t last_printed_time;

uint8_t configButton = 0;
uint32_t buttonCounter;
volatile uint8_t state = 0, dotPosition = 0b10;
NTPSyncEvent_t ntpEvent;
String serialCommand = "";

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
	Serial.println("\33[2K\r\nNixie Tap is booting!");

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

	// Setup WiFi station mode settings and begin connection attempt.
	setupWiFi();
	connectWiFi();

	// Load time zone.
	loadTimeZone();

	// Progress bar: 75%.
	nixieTap.write(10, 10, 10, 10, 0b1110);

	// Set the system time from the on-board RTC.
	RTC.begin(D3, D4);
	RTC.setCharger(2);
	setSystemTimeFromRTC();
	printTime(now());

	enableSecDot();

	// Progress bar: 100%.
	nixieTap.write(10, 10, 10, 10, 0b11110);
}

void loop()
{
	// Handle an event triggered from the NTP client.
	if (syncEventTriggered) {
		processSyncEvent(ntpEvent);
		syncEventTriggered = false;
	}

	// Get the current time and calculate its offset from UTC.
	current_time = now();
	int32_t offset = ZonedDateTime::forUnixSeconds64(current_time, time_zone).timeOffset().toSeconds();

	// State machine.
	if (state > 1) {
		state = 0;
	}

	// Slot 0 - time
	if (state == 0) {
		nixieTap.writeTime(current_time + offset, dot_state, cfg_24hr_enabled);
	}

	// Slot 1 - date
	if (state == 1) {
		nixieTap.writeDate(current_time + offset, 1);
	}

	// Print the current time if the touch sensor was pressed.
	if (touch_button_pressed) {
		touch_button_pressed = false;
		printTime(current_time);
	}

	// Print the current time if the serial ticker is enabled.
	if (serialTicker) {
		printTime(current_time);
	}

	// Handle serial interface input.
	readAndParseSerial();

	// Handle config button presses.
	readConfigButton();
}

void setupWiFi()
{
	WiFi.mode(WIFI_STA);
	WiFi.hostname("NixieTap");
	WiFi.persistent(false);
	WiFi.setAutoReconnect(true);

	static WiFiEventHandler eh_sta_dhcp_timeout =
		WiFi.onStationModeDHCPTimeout([](void)
	{
		Serial.println("[Wi-Fi] DHCP timeout");
	});

	static WiFiEventHandler eh_sta_got_ip =
		WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& event)
	{
		Serial.print("[Wi-Fi] DHCP succeeded, IP address ");
		Serial.print(WiFi.localIP());
		Serial.print(", subnet mask ");
		Serial.print(WiFi.subnetMask());
		Serial.print(", gateway ");
		Serial.print(WiFi.gatewayIP());
		Serial.print(", DNS ");
		Serial.println(WiFi.dnsIP());

		// Start the NTP client if enabled.
		startNTPClient();
	});

	static WiFiEventHandler eh_sta_auth_mode_changed =
		WiFi.onStationModeAuthModeChanged([](const WiFiEventStationModeAuthModeChanged& event)
	{
		static const char * const AUTH_MODE_NAMES[] {
			"AUTH_OPEN",
			"AUTH_WEP",
			"AUTH_WPA_PSK",
			"AUTH_WPA2_PSK",
			"AUTH_WPA_WPA2_PSK",
			"AUTH_MAX"
		};
		Serial.print("[Wi-Fi] Authentication mode changed, old mode ");
		Serial.print(AUTH_MODE_NAMES[event.oldMode]);
		Serial.print(", new mode ");
		Serial.println(AUTH_MODE_NAMES[event.newMode]);
	});

	static WiFiEventHandler eh_sta_connected =
		WiFi.onStationModeConnected([](const WiFiEventStationModeConnected& event)
	{
		Serial.print("[Wi-Fi] Station connected, SSID \"");
		Serial.print(WiFi.SSID());
		Serial.print("\", channel ");
		Serial.print(event.channel);
		Serial.print(", RSSI ");
		Serial.print(WiFi.RSSI());
		Serial.print(" dBm, BSSID ");
		Serial.println(WiFi.BSSIDstr());
	});

	static WiFiEventHandler eh_sta_disconnected =
		WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event)
	{
		Serial.print("[Wi-Fi] Station disconnected, reason: ");
		Serial.print(wifiDisconnectReasonStr(event.reason));
		Serial.print(" (");
		Serial.print((unsigned)event.reason);
		Serial.println(")");

		// Stop the NTP client if it's running.
		stopNTPClient();
	});
}

void connectWiFi()
{
	WiFi.disconnect();

	if (cfg_ssid[0] == '\0' || cfg_password[0] == '\0') {
		return;
	}

	WiFi.begin(cfg_ssid, cfg_password);

	Serial.print("[Wi-Fi] Connecting to access point: ");
	Serial.println(cfg_ssid);
}

void loadTimeZone()
{
	time_zone = zoneManager.createForZoneName(cfg_time_zone);
	if (!time_zone.isError()) {
		Serial.print("[Time] Loaded time zone: ");
		Serial.println(cfg_time_zone);
	} else {
		Serial.println("[Time] Unable to load time zone, using UTC.");

		// Use UTC instead.
		time_zone = zoneManager.createForZoneInfo(&zonedbx::kZoneEtc_UTC);
		if (time_zone.isError()) {
			Serial.println("[Time] WARNING! Unable to load UTC time zone.");
		}
	}
}

void setSystemTimeFromRTC()
{
	setTime(RTC.get());
	Serial.println("[Time] System time has been set from the on-board RTC.");
}

void startNTPClient()
{
	if (cfg_ntp_enabled != 1) {
		return;
	}

	if (ntpInitialized) {
		Serial.println("[NTP] Restarting NTP client.");
		NTP.stop();
		ntpInitialized = false;
	} else {
		Serial.println("[NTP] Starting NTP client.");
	}

	NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {
		ntpEvent = event;
		syncEventTriggered = true;
	});

	if (!NTP.setInterval(cfg_ntp_sync_interval)) {
		Serial.println("[NTP] Failed to set sync interval!");
	}

	if (NTP.begin(cfg_ntp_server)) {
		ntpInitialized = true;
	} else {
		Serial.println("[NTP] Failed to start NTP client!");
	}
}

void stopNTPClient()
{
	if (ntpInitialized) {
		Serial.println("[NTP] Stopping NTP client.");
		NTP.stop();
		ntpInitialized = false;
	}
}

void processSyncEvent(NTPSyncEvent_t ntpEvent)
{
	if (ntpEvent < 0) {
		Serial.print("[NTP] Time sync error: ");
		if (ntpEvent == noResponse) {
			Serial.println("NTP server not reachable.");
		} else if (ntpEvent == invalidAddress) {
			Serial.println("Invalid NTP server address.");
		} else if (ntpEvent == errorSending) {
			Serial.println("Error sending request.");
		} else if (ntpEvent == responseError) {
			Serial.println("NTP response error.");
		} else {
			Serial.println("Unknown event.");
		}
	} else {
		if (ntpEvent == timeSyncd && NTP.SyncStatus()) {
			time_t ntp_time = NTP.getLastNTPSync();
			RTC.set(ntp_time);
			printTime(ntp_time);
		}
	}
}

/*
 * Enable the center dot to change its state every second.
 */
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

/*
 * An interrupt function for changing the dot state every second.
 */
void irq_1Hz_int()
{
	dot_state = !dot_state;
}

/*
 * An interrupt function for the touch sensor when it is touched.
 */
void touchButtonPressed()
{
	state++;
	touch_button_pressed = true;
	nixieTap.setAnimation(true);
}

void readAndParseSerial()
{
	if (Serial.available() > 0) {
		serialCommand.concat(Serial.readStringUntil('\n'));

		if (serialCommand.endsWith("\r")) {
			serialCommand.trim();

			if (serialCommand == "espinfo") {
				printESPInfo();
			} else if (serialCommand == "init") {
				resetEepromToDefault();
			} else if (serialCommand == "read") {
				readParameters();
			} else if (serialCommand == "restart") {
				Serial.println("Nixie Tap is restarting!");
				EEPROM.commit();
				ESP.restart();
			} else if (serialCommand == "set") {
				Serial.println("Available 'set' commands: "
					       "24hr_enabled, "
					       "ntp_enabled, "
					       "ntp_sync_interval, "
					       "ntp_server, "
					       "time_zone, "
					       "ssid, "
					       "password, "
					       "time.");
			} else if (serialCommand.startsWith("set ")) {
				parseSerialSet(serialCommand.substring(strlen("set ")));
			} else if (serialCommand == "ticker") {
				if (serialTicker) {
					Serial.println("[Time] Turning off serial ticker.");
				} else {
					Serial.println("[Time] Turning on serial ticker.");
				}
				serialTicker = !serialTicker;
			} else if (serialCommand == "time") {
				printTime(now());
			} else if (serialCommand == "write") {
				EEPROM.commit();
				Serial.println("[EEPROM Commit] Writing settings to non-volatile memory.");
			} else if (serialCommand == "help") {
				Serial.println("Available commands: "
					       "espinfo, "
					       "init, "
					       "read, "
					       "restart, "
					       "set, "
					       "ticker, "
					       "time, "
					       "write, "
					       "help.");
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
	if (s.startsWith("24hr_enabled ")) {
		uint8_t val = (uint8_t)atoi(s.substring(strlen("24hr_enabled ")).c_str());
		cfg_24hr_enabled = val;
		Serial.print("[EEPROM Write] ");
		Serial.print("24hr_enabled: ");
		Serial.println(val);
		EEPROM.put(EEPROM_ADDR__24HR_ENABLED, val);
	} else if (s.startsWith("ntp_enabled ")) {
		uint8_t val = (uint8_t)atoi(s.substring(strlen("ntp_enabled ")).c_str());
		cfg_ntp_enabled = val;
		Serial.print("[EEPROM Write] ");
		Serial.print("ntp_enabled: ");
		Serial.println(val);
		EEPROM.put(EEPROM_ADDR__NTP_ENABLED, val);

		// Stop or start the NTP client.
		if (cfg_ntp_enabled == 0 && ntpInitialized) {
			stopNTPClient();
		} else if (cfg_ntp_enabled == 1 && !ntpInitialized) {
			startNTPClient();
		}
	} else if (s.startsWith("ntp_sync_interval ")) {
		uint32_t val = (uint8_t)atoi(s.substring(strlen("ntp_sync_interval ")).c_str());
		cfg_ntp_sync_interval = val;
		Serial.print("[EEPROM Write] ");
		Serial.print("ntp_sync_interval: ");
		Serial.println(val);
		EEPROM.put(EEPROM_ADDR__NTP_SYNC_INTERVAL, val);

		// Restart the NTP client if necessary.
		if (cfg_ntp_enabled && ntpInitialized) {
			startNTPClient();
		}
	} else if (s.startsWith("ntp_server ")) {
		strcpy(cfg_ntp_server, s.substring(strlen("ntp_server ")).c_str());
		Serial.print("[EEPROM Write] ");
		Serial.print("ntp_server: ");
		Serial.println(cfg_ntp_server);
		EEPROM.put(EEPROM_ADDR__NTP_SERVER, cfg_ntp_server);

		// Restart the NTP client if necessary.
		if (cfg_ntp_enabled && ntpInitialized) {
			startNTPClient();
		}
	} else if (s.startsWith("time_zone ")) {
		strcpy(cfg_time_zone, s.substring(strlen("time_zone ")).c_str());
		Serial.print("[EEPROM Write] ");
		Serial.print("time_zone: ");
		Serial.println(cfg_time_zone);
		EEPROM.put(EEPROM_ADDR__TIME_ZONE, cfg_time_zone);

		// Reload time zone.
		loadTimeZone();
	} else if (s.startsWith("ssid ")) {
		strcpy(cfg_ssid, s.substring(strlen("ssid ")).c_str());
		Serial.print("[EEPROM Write] ");
		Serial.print("ssid: ");
		Serial.println(cfg_ssid);
		EEPROM.put(EEPROM_ADDR__SSID, cfg_ssid);

		// Restart WiFi connection because the SSID has changed.
		connectWiFi();
	} else if (s.startsWith("password ")) {
		strcpy(cfg_password, s.substring(strlen("password ")).c_str());
		Serial.print("[EEPROM Write] ");
		Serial.print("password: ");
		Serial.println(cfg_password);
		EEPROM.put(EEPROM_ADDR__PASSWORD, cfg_password);

		// Restart WiFi connection because the password has changed.
		connectWiFi();
	} else if (s.startsWith("time ")) {
		String s_time = s.substring(strlen("time "));
		auto odt = OffsetDateTime::forDateString(s_time.c_str());
		if (!odt.isError()) {
			time_t odt_unix = odt.toUnixSeconds64();
			setTime(odt_unix);
			RTC.set(odt_unix);
			last_printed_time = 0;
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

void printESPInfo()
{
	Serial.print("[ESP] Boot mode: ");
	Serial.println(ESP.getBootMode());

	Serial.print("[ESP] Boot version: ");
	Serial.println(ESP.getBootVersion());

	Serial.print("[ESP] Reset reason: ");
	Serial.println(ESP.getResetReason());

	Serial.print("[ESP] Reset info: ");
	Serial.println(ESP.getResetInfo());

	Serial.print("[ESP] Free heap: ");
	Serial.println(ESP.getFreeHeap());

	Serial.print("[ESP] Heap fragmentation: ");
	Serial.println(ESP.getHeapFragmentation());

	Serial.print("[ESP] Max free block size: ");
	Serial.println(ESP.getMaxFreeBlockSize());

	Serial.print("[ESP] Chip ID: ");
	Serial.println(ESP.getChipId());

	Serial.print("[ESP] Core version: ");
	Serial.println(ESP.getCoreVersion());

	Serial.print("[ESP] Full version: ");
	Serial.println(ESP.getFullVersion());

	Serial.print("[ESP] SDK version: ");
	Serial.println(ESP.getSdkVersion());

	Serial.print("[ESP] CPU frequency MHz: ");
	Serial.println(ESP.getCpuFreqMHz());

	Serial.print("[ESP] Sketch size: ");
	Serial.println(ESP.getSketchSize());

	Serial.print("[ESP] Free sketch space: ");
	Serial.println(ESP.getFreeSketchSpace());

	Serial.print("[ESP] Sketch MD5: ");
	Serial.println(ESP.getSketchMD5());

	Serial.print("[ESP] Flash chip ID: ");
	Serial.println(ESP.getFlashChipId());

	Serial.print("[ESP] Flash chip size: ");
	Serial.println(ESP.getFlashChipSize());

	Serial.print("[ESP] Flash chip speed: ");
	Serial.println(ESP.getFlashChipSpeed());
}

void printTime(time_t t)
{
	if (t > last_printed_time) {
		Serial.print("[Time] The time is now: ");
		ZonedDateTime::forUnixSeconds64(t, time_zone).printTo(Serial);
		Serial.print(" @ ");
		Serial.println(t);
		last_printed_time = t;
	}
}

void readParameters()
{
	Serial.println("[EEPROM] Reading settings from non-volatile memory.");

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
	Serial.println("[EEPROM] Writing defaults to non-volatile memory.");

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

void readConfigButton()
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
		Serial.println("[EEPROM] Magic value mismatch.");
		resetEepromToDefault();
	}
}

const char *wifiDisconnectReasonStr(const enum WiFiDisconnectReason reason)
{
	switch (reason) {
	case WIFI_DISCONNECT_REASON_UNSPECIFIED: return "WIFI_DISCONNECT_REASON_UNSPECIFIED";
	case WIFI_DISCONNECT_REASON_AUTH_EXPIRE: return "WIFI_DISCONNECT_REASON_AUTH_EXPIRE";
	case WIFI_DISCONNECT_REASON_AUTH_LEAVE: return "WIFI_DISCONNECT_REASON_AUTH_LEAVE";
	case WIFI_DISCONNECT_REASON_ASSOC_EXPIRE: return "WIFI_DISCONNECT_REASON_ASSOC_EXPIRE";
	case WIFI_DISCONNECT_REASON_ASSOC_TOOMANY: return "WIFI_DISCONNECT_REASON_ASSOC_TOOMANY";
	case WIFI_DISCONNECT_REASON_NOT_AUTHED: return "WIFI_DISCONNECT_REASON_NOT_AUTHED";
	case WIFI_DISCONNECT_REASON_NOT_ASSOCED: return "WIFI_DISCONNECT_REASON_NOT_ASSOCED";
	case WIFI_DISCONNECT_REASON_ASSOC_LEAVE: return "WIFI_DISCONNECT_REASON_ASSOC_LEAVE";
	case WIFI_DISCONNECT_REASON_ASSOC_NOT_AUTHED: return "WIFI_DISCONNECT_REASON_ASSOC_NOT_AUTHED";
	case WIFI_DISCONNECT_REASON_DISASSOC_PWRCAP_BAD: return "WIFI_DISCONNECT_REASON_DISASSOC_PWRCAP_BAD";
	case WIFI_DISCONNECT_REASON_DISASSOC_SUPCHAN_BAD: return "WIFI_DISCONNECT_REASON_DISASSOC_SUPCHAN_BAD";
	case WIFI_DISCONNECT_REASON_IE_INVALID: return "WIFI_DISCONNECT_REASON_IE_INVALID";
	case WIFI_DISCONNECT_REASON_MIC_FAILURE: return "WIFI_DISCONNECT_REASON_MIC_FAILURE";
	case WIFI_DISCONNECT_REASON_4WAY_HANDSHAKE_TIMEOUT: return "WIFI_DISCONNECT_REASON_4WAY_HANDSHAKE_TIMEOUT";
	case WIFI_DISCONNECT_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "WIFI_DISCONNECT_REASON_GROUP_KEY_UPDATE_TIMEOUT";
	case WIFI_DISCONNECT_REASON_IE_IN_4WAY_DIFFERS: return "WIFI_DISCONNECT_REASON_IE_IN_4WAY_DIFFERS";
	case WIFI_DISCONNECT_REASON_GROUP_CIPHER_INVALID: return "WIFI_DISCONNECT_REASON_GROUP_CIPHER_INVALID";
	case WIFI_DISCONNECT_REASON_PAIRWISE_CIPHER_INVALID: return "WIFI_DISCONNECT_REASON_PAIRWISE_CIPHER_INVALID";
	case WIFI_DISCONNECT_REASON_AKMP_INVALID: return "WIFI_DISCONNECT_REASON_AKMP_INVALID";
	case WIFI_DISCONNECT_REASON_UNSUPP_RSN_IE_VERSION: return "WIFI_DISCONNECT_REASON_UNSUPP_RSN_IE_VERSION";
	case WIFI_DISCONNECT_REASON_INVALID_RSN_IE_CAP: return "WIFI_DISCONNECT_REASON_INVALID_RSN_IE_CAP";
	case WIFI_DISCONNECT_REASON_802_1X_AUTH_FAILED: return "WIFI_DISCONNECT_REASON_802_1X_AUTH_FAILED";
	case WIFI_DISCONNECT_REASON_CIPHER_SUITE_REJECTED: return "WIFI_DISCONNECT_REASON_CIPHER_SUITE_REJECTED";
	case WIFI_DISCONNECT_REASON_BEACON_TIMEOUT: return "WIFI_DISCONNECT_REASON_BEACON_TIMEOUT";
	case WIFI_DISCONNECT_REASON_NO_AP_FOUND: return "WIFI_DISCONNECT_REASON_NO_AP_FOUND";
	case WIFI_DISCONNECT_REASON_AUTH_FAIL: return "WIFI_DISCONNECT_REASON_AUTH_FAIL";
	case WIFI_DISCONNECT_REASON_ASSOC_FAIL: return "WIFI_DISCONNECT_REASON_ASSOC_FAIL";
	case WIFI_DISCONNECT_REASON_HANDSHAKE_TIMEOUT: return "WIFI_DISCONNECT_REASON_HANDSHAKE_TIMEOUT";
	default:
		return "Unknown";
	}
}
