This is a replacement firmware for the ESP8266-based [Nixie Tap](https://mladendinic.com/nixietap/) desktop clock which uses Soviet ИН-12Б nixie tube decimal indicators to display the current time and date. The main improvement of this firmware is that it automatically handles DST transitions using the [AceTime](https://github.com/bxparks/AceTime) library, simply by configuring the local time zone into the EEPROM settings.

It has the following changes from the [original firmware](https://github.com/mladendinic/nixietap/tree/master/firmware):

* Remove cryptocurrency junk.
* Remove various location and time zone API clients (Google Location, Google Timezone, ipify, seeip, IPStack, ip-api, timezonedb.com). The original firmware attempts to obtain the clock's geolocation and then use the geolocation to determine a time zone automatically by calling various HTTP APIs, some of which require API keys. None of these APIs are needed because this firmware has a built-in time zone database and the local time zone is configured explicitly by the clock's user.
* Remove weather/temperature API client and temperature display mode.
* Remove browser-based captive portal configuration. All configuration is performed via the serial interface.
* Remove manual time zone offset configuration. The local time zone is configured explicitly by name.
* Add automatic time zone offset calculation and DST transition using the AceTime library.
* Run the SNTP client continuously rather than once at boot so that the system time doesn't drift.
* Only sync the system time from the RTC at boot. Afterwards, the system time is further updated upon successful SNTP updates from the network.
* Print the current timestamp in ISO8601 format and in Unix epoch seconds to the serial port when the touch sensor is pressed and upon a successful SNTP update from the network.
* Set the DHCP client hostname to `NixieTap` rather than using the default, generic `ESP_XXXXXX` value.
* Show the month and day in the correct order (MMDD, not DDMM) in date display mode.

Even though this firmware includes an extensive built-in time zone database, it is still almost 30% smaller than the original firmware due to the removal of the various API clients and the captive portal.

The serial interface accepts input commands. Make sure to turn on local echo in your serial terminal emulator, e.g. `picocom -c -b 115200 /dev/ttyUSB0`. The following commands are supported via the serial interface:

* `init`: Reinitialize the EEPROM settings to default values.
* `read`: Read and display the current EEPROM settings.
* `restart`: Perform a warm restart of the Nixie Tap.
* `set`: Change a setting.
* `time`: Print the current system time in ISO8601 format and in Unix epoch seconds.
* `write`: Save the configuration values changed with `set` to the EEPROM.
* `help`: Print the list of recognized commands.

The following EEPROM settings may be set via the serial interface using the `set` command:

* `24hr_enabled`: Whether to format the time using 12 or 24 hour format.
* `ntp_enabled`: Whether the SNTP client is enabled or not.
* `ntp_server`: The hostname of the NTP server to use.
* `ntp_sync_interval`: The interval between SNTP updates, in seconds.
* `time_zone`: The name of the time zone to use, e.g. "America/New_York".
* `ssid`: The SSID of the Wi-Fi network to connect to.
* `password`: The passphrase of the Wi-Fi network to connect to.

Additionally, the `set time` command can be used to set both the current system time as well as the time stored in the on-board RTC. The timestamp supplied to the `set time` command must be in ISO8601 format.

Reasonable defaults are configured into the initial EEPROM contents except for the `ssid` and `password` values which must be set in order to bring the Nixie Tap online.

To configure all of the available settings the following commands could be used:
```
set 24hr_enabled 1
set ntp_enabled 1
set ntp_sync_interval 7207
set ntp_server time.apple.com
set time_zone Europe/Amsterdam
set ssid [...The network's SSID...]
set password [...The network's passphrase...]
write
restart
```

To watch a DST transition the following commands can be used:
```
set ntp_enabled 0
set time_zone America/Denver
write
set time 2023-11-05T01:59:45-06:00
restart
```

Using the above commands, the output on the serial interface during a DST transition should look something like this:
```
$ picocom -q -c -b 115200 /dev/ttyUSB0
set ntp_enabled 0
[EEPROM Write] ntp_enabled: 0
set time_zone America/Denver
[EEPROM Write] time_zone: America/Denver
write
[EEPROM Commit]
set time 2023-11-05T01:59:45-06:00
The time is now: 2023-11-05T01:59:45-06:00[America/Denver] @ 1699171185
restart
Nixie Tap is restarting!

 ets Jan  8 2013,rst cause:2, boot mode:(3,7)

load 0x4010f000, len 3424, room 16
tail 0
chksum 0x2e
load 0x3fff20b8, len 40, room 8
tail 0
chksum 0x2b
csum 0x2b
v00055ce0
~ld


Nixie Tap is booting!
Reading saved parameters from EEPROM...
[EEPROM Read] 24hr_enabled: 1
[EEPROM Read] ntp_enabled: 0
[EEPROM Read] ntp_sync_interval: 3671
[EEPROM Read] ntp_server: time.google.com
[EEPROM Read] time_zone: America/Denver
[EEPROM Read] ssid: [...]
[EEPROM Read] password: [...]
System time has been set from the on-board RTC.
The time is now: 2023-11-05T01:59:48-06:00[America/Denver] @ 1699171188
Connecting to Wi-Fi access point: [...]
Connected, IP address: 10.1.11.141
The time is now: 2023-11-05T01:59:53-06:00[America/Denver] @ 1699171193
The time is now: 2023-11-05T01:59:54-06:00[America/Denver] @ 1699171194
The time is now: 2023-11-05T01:59:55-06:00[America/Denver] @ 1699171195
The time is now: 2023-11-05T01:59:56-06:00[America/Denver] @ 1699171196
The time is now: 2023-11-05T01:59:57-06:00[America/Denver] @ 1699171197
The time is now: 2023-11-05T01:59:58-06:00[America/Denver] @ 1699171198
The time is now: 2023-11-05T01:59:59-06:00[America/Denver] @ 1699171199
The time is now: 2023-11-05T01:00:02-07:00[America/Denver] @ 1699171202
The time is now: 2023-11-05T01:00:02-07:00[America/Denver] @ 1699171202
The time is now: 2023-11-05T01:00:03-07:00[America/Denver] @ 1699171203
The time is now: 2023-11-05T01:00:04-07:00[America/Denver] @ 1699171204
```

The nixie tube indicators should display the corresponding time change from 01:59 to 01:00 during the DST transition. Note that the timestamps being printed in the output above are caused by physically tapping on the touch sensor on the top of the Nixie Tap.

The firmware is built using [PlatformIO Core](https://docs.platformio.org/en/latest/core/index.html) by calling the `pio run` command. Branch pushes and pull requests will trigger a CI build using GitHub Actions. Pushing a tag will additionally upload the CI built firmware to the [Releases](https://github.com/edmonds/nixietap/releases) page.
