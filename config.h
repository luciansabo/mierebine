// Pins
#define CONFIG_PORTAL_PIN       0
#define BOILER_RELAY_PIN        4
#define HCE80_RELAY_INPUT_PIN   5

// Timing
#define RELAY_DELAY             (90 * 1000) // 90s in ms
#define MAX_ON_TIME             (6 * 3600 * 1000) // 6h in ms
#define MQTT_WATCHDOG_INTERVAL  (60 * 1000) // 1m
#define MQTT_HANDLING_INTERVAL  (1 * 1000) // 1s
#define MQTT_KEEP_ALIVE_SEC     60 // 60s

// Config portal
#define CONFIG_PORTAL_AP_NAME   "MiereBine"
#define CONFIG_PORTAL_AP_PASS   ""
#define CONFIG_PORTAL_TIMEOUT   (10 * 60 * 1000) // 10 min

// Other
#define HOSTNAME                "MiereBine"