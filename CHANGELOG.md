2022.10
- separated RELAY_DELAT into RELAY_ON_DELAY and RELAY_OFF_DELAY
- added two new config variables MIN_ON_TIME and MIN_ON_TIME_WINDOW for optimizing short cycles
- mqtt connection uses the retain flag for the availability topic. This improves HomeAssistant integration
- removed runs from status page, added lastGoodOffTime

2022.9
- changed min on time to 2m, fixed broken code due to missing variables

2022.2
- improved reliability by removing ISR interrupts and using an ISR timer with a debouncing library

2022.1
- initial release
