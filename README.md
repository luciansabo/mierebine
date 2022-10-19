## MiereBine (Pardosilla)

This little ESP8266 based device tries to solve the software issues
from Honeywell Evohome smart thermostat when used in conjuction the HCE80 underfloor heating controller

**The problems:**
- If the Evohome controls BDR91 (the boiler relay) it has some settings like cycle  time and min on time
While this works, it doesn't seem to be synced with the actuators open time that HCE80HCC80 should control
The outcome is that BDR91 fires at fixed intervals as scheduled (let's say 6 times in an hour) but the UFH actuators
may or may not be open. This means the boiler may or may not have circuits to heat - useless gas consumptions but no heating
- If the Evohome is directly paired with HCE80, they seem to ignore the cycle time and min on time and BDR91 will fire according
to whateber heat demand Evohome calculates, but then the boiler cycles too often. For example it will might start a 5 min cycle
then 1 min pause then another 1 minute cycle.
The worst thing is that they are starting the relay with a 30sec delay (the moment the pump relay kicks in). This delay is not configurable.
The problem is that they sell actuators with much longer open time. It is somewhere between 1m30s and 4m depending on cold/warm start.
That 30s delay is not enough to open any actuator, but the boiler relay is triggered and it starts consuming has while no circuit is yet open.
After 1m30s after the relay is triggered the actuators finally open, but we just wasted 1m30s worth of gas.
The relay is stopped imediatally not with a delay, even if the still opened circuit could accept another minute of heat.

When directly paired with HCE80 it does seem to proportionally open the actuators and start the boiler wnen the actuators start opening, but it does this even of shorter periods than the min on time.
So you might not have heat demand but the HCE80 and BDR91 will be started. This leads of course to overshoot and short cycles (even less than 1 min).


All these are of course a bugs. I contacted Honeywell support and they failed to understand the issue
and insisted their device works properly even though it was really clear for me that they don't know exactly how it should behave. 
They look at Evohome as some kind of magic device and insist that the TPI algorithm will finally "learn" the house. It doesn't. :)
It is just nice hardware with crappy software.
So, to actually make some use out of this setup, I needed to fix the damn relay and make it sync with my actuators.

**Rules:**
- start and stop boiler relay with a configurable delay (2m for Honeywell actuators works fine, because there is an extra 30s delay before relay fires). The off delay is set to 1m30s
- if the on interval is less than the minimum allowed (RELAY_ON_DELAY) do not even bother to start the boiler relay
- if the source relay has switched on again during our wait we can keep the relay on
- prevent the short cycles and overshoots by detecting that the source relay was working less than MIN_ON_TIME (set by default at 4m) in the MIN_ON_TIME_WINDOW (by default 15m). This protection kicks in after the first short cycle detected and is disabled after the first good cycle detected.
A good cycle is either at least 4m in the last 15m or we had more than 15m since the last event
- if the short cycle protection applies but a longer cycle is detected (double the MIN_ON_TIME - should be 8m), while the protection prevents the boiler relay from starting, we start the boiler
- stop the boiler relay after x hours of constant runtime as safeguard

### Hardware

- Any ESP8266 based board such as NodeMCU or Wemos D1
- Mechanical relay module

### Features
- Makes Honeywell EvoHome work ... honey-well
- Configurable delays and timings
- Using hardware ISR timer for the core switching function for safer/precise operations
- On demand config portal for wifi settings
- Publishes status to MQTT. Configurable MQTT connection watchdog.
- Circular memory log with NTP time for last 300 events. Inspect the log using a http client
- OTA firmware updates (signed)
- mDNS host enabled

