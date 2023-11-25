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
* Print the current timestamp in ISO8601 format and in Unix epoch seconds to the serial port when the touch sensor is pressed and upon a successful SNTP update from the network. Continuous printing of the current time can be toggled using the `ticker` command.
* Set the DHCP client hostname to `NixieTap` rather than using the default, generic `ESP_XXXXXX` value.
* Show the month and day in the correct order (MMDD, not DDMM) in date display mode.

Even though this firmware includes an extensive built-in time zone database, it is still about 25% smaller than the original firmware due to the removal of the various API clients and the captive portal.

The serial interface accepts input commands. Make sure to turn on local echo in your serial terminal emulator, e.g. `picocom -c -b 115200 /dev/ttyUSB0`. The following commands are supported via the serial interface:

* `espinfo`: Print various system information using the ESP API.
* `init`: Reinitialize the EEPROM settings to default values.
* `read`: Read and display the current EEPROM settings.
* `restart`: Save any changed EEPROM settings and perform a warm restart of the Nixie Tap.
* `set`: Change a setting.
* `set time`: Manually set the system time.
* `ticker`: Print the current time once a second.
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

The `set time` command can be used to set both the current system time and the time stored in the on-board RTC. The timestamp supplied to the `set time` command must be in ISO8601 format.

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
restart
```

To watch a DST transition the following commands can be used:
```
set ntp_enabled 0
set time_zone America/Denver
set time 2023-11-05T01:59:50-06:00
ticker
```

Using the above commands, the output on the serial interface during a DST transition should look something like this:
```
$ picocom -q -c -b 115200 /dev/ttyUSB0
set ntp_enabled 0
[EEPROM Write] ntp_enabled: 0
set time_zone America/Denver
[EEPROM Write] time_zone: America/Denver
[Time] Loaded time zone: America/Denver
set time 2023-11-05T01:59:50-06:00
[Time] The time is now: 2023-11-05T01:59:50-06:00[America/Denver] @ 1699171190
ticker
[Time] Turning on serial ticker.
[Time] The time is now: 2023-11-05T01:59:53-06:00[America/Denver] @ 1699171193
[Time] The time is now: 2023-11-05T01:59:54-06:00[America/Denver] @ 1699171194
[Time] The time is now: 2023-11-05T01:59:55-06:00[America/Denver] @ 1699171195
[Time] The time is now: 2023-11-05T01:59:56-06:00[America/Denver] @ 1699171196
[Time] The time is now: 2023-11-05T01:59:57-06:00[America/Denver] @ 1699171197
[Time] The time is now: 2023-11-05T01:59:58-06:00[America/Denver] @ 1699171198
[Time] The time is now: 2023-11-05T01:59:59-06:00[America/Denver] @ 1699171199
[Time] The time is now: 2023-11-05T01:00:00-07:00[America/Denver] @ 1699171200
[Time] The time is now: 2023-11-05T01:00:02-07:00[America/Denver] @ 1699171202
[Time] The time is now: 2023-11-05T01:00:03-07:00[America/Denver] @ 1699171203
[Time] The time is now: 2023-11-05T01:00:04-07:00[America/Denver] @ 1699171204
ticker
[Time] Turning off serial ticker.
```

The nixie tube indicators should show the time changing from 01:59 to 01:00 across the DST transition. Note that the missing second in the output above at 01:00:01 is due to the use of [`delay()`](https://www.arduino.cc/reference/en/language/functions/time/delay/) in the anti-poisoning animation code, which adds about 1250 milliseconds of delay. (The use of `delay()` apparently also prevents the use of the [ESPNtpClient](https://github.com/gmag11/ESPNtpClient) library.)

The firmware is built using [PlatformIO Core](https://docs.platformio.org/en/latest/core/index.html) by calling the `pio run` command. Branch pushes and pull requests will trigger a CI build using GitHub Actions. Pushing a tag will additionally upload the CI built firmware to the [Releases](https://github.com/edmonds/nixietap/releases) page.
