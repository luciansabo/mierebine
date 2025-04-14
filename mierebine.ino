/*
 * MiereBine (Pardosilla) v 2022.2
 * 
 * This little ESP8266 based device tries to solve the software issues
 * from Honeywell Evohome smart thermostat when used in conjuction the HCE80 underfloor heating controller
 * 
 * Made for NodeMCU but should work as is with any ESP8266 board and probably with ESP32 too
 * 
 * Arduino IDE Config
 * Board: NodeMCU 1.0 (ESP 12E)
 * CPU Freq: 80 Mhz 
 * 
 */

#include <Bounce2.h> // https://github.com/thomasfredericks/Bounce2
#include <ESP8266TimerInterrupt.h> // https://github.com/khoih-prog/ESP8266TimerInterrupt
#include <SimpleTimer.h> // https://playground.arduino.cc/Code/SimpleTimer/
#include <WiFiManager.h> 
#include <TelnetStream.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <coredecls.h>                  // settimeofday_cb()

// app config
#include "config.h"
#include "config.local.h"

#ifndef DEBUG
#define Serial if(0)Serial
#endif

// input relay is mechanical so we need to debounce it
Bounce inputRelay = Bounce();

// software timer
SimpleTimer simpleTimer;
volatile int timerId, maxOnTimeTimer;

// hardware timer
ESP8266Timer hwTimer;

// network
WiFiClient espClient;
// servers
PubSubClient mqtt(espClient);

// Wifimanager variables
WiFiManager wifiManager;
bool portalRunning      = false;
ulong configPortalStartTime = millis();

volatile bool hasExternalHeatDemand = false, hasInternalHeatDemand = false;

// on/off stats
ulong lastInputRelayOnTime = 0, lastInputRelayOffTime = 0, lastGoodOffTime;

struct TStats {
  time_t timeStarted;
  ulong lastGoodOffTime = 0;
} stats;

char mqttBroker[100] = DEFAULT_MQTT_BROKER;
const char *outputRelayTopic = "saboiot/mierebine/outputRelay";
const char *inputRelayTopic = "saboiot/mierebine/inputRelay";
const char *availabilityTopic = "saboiot/mierebine/availability";

// ------------------------------------------------------------------------------------------

bool isBoilerOn() {  
  return !digitalRead(BOILER_RELAY_PIN);  
}

// ------------------------------------------------------------------------------------------

void mqttPublish(const char *topic, const char *message, bool retain = false) {  
  ensureMqttConnected();

  if (!mqtt.publish(topic, message, retain)) {    
    Serial.printf("MQTT Publish failed. Topic: " );
    Serial.print(topic);
    Serial.print(" Msg: ");
    Serial.print(message);
    Serial.print(" State: ");
    Serial.println(mqtt.state());
  }
}

// ------------------------------------------------------------------------------------------

void publishInputRelayStatus() {
  if (isInputRelayOn()) {
    mqttPublish(inputRelayTopic, "{\"state\": \"ON\"}");
  } else {
    mqttPublish(inputRelayTopic, "{\"state\": \"OFF\"}");    
  }
}

// ------------------------------------------------------------------------------------------

bool isInputRelayOn() {
  return inputRelay.read() == LOW;
}

// ------------------------------------------------------------------------------------------

void onRuntimeProtection() {  
  turnOff();    
}
// ------------------------------------------------------------------------------------------

void turnOn(int origin = 0) {
  if (origin == 0) {
    hasInternalHeatDemand = true;
  }
  digitalWrite(BOILER_RELAY_PIN, LOW); // turn boiler relay on    
  //maxOnTimeTimer = simpleTimer.setTimeout(MAX_ON_TIME, onRuntimeProtection);    
  
  publishOutputRelayStatus();
}  

void openActuator() {
  digitalWrite(ACTUATOR_RELAY_PIN, LOW);
}

void closeActuator() {
  digitalWrite(ACTUATOR_RELAY_PIN, HIGH);
}


// ------------------------------------------------------------------------------------------

void turnOff() {
  //simpleTimer.deleteTimer(maxOnTimeTimer);    

  if (hasExternalHeatDemand) {
    TelnetStream.println("Boiler has external heat demand. Ignoring turn off");
    return;
  }
   
  if (!isBoilerOn()) {
    TelnetStream.println("Boiler is not on. Ignoring turn off");
    return;
  }

  TelnetStream.println("Turn boiler off");
  
  digitalWrite(BOILER_RELAY_PIN, HIGH); // turn boiler relay off        
      
  publishOutputRelayStatus();  
}

// ------------------------------------------------------------------------------------------

void IRAM_ATTR hwTimerHandler()
{
  inputRelay.update(); 
  
  if (inputRelay.changed()) {
    
    // remove the old timer to avoid triggering on/off
    // if the source relay toggled before the timer interval  
    
    simpleTimer.deleteTimer(timerId);        
  
    if (isInputRelayOn()) {      
      simpleTimer.setTimeout(100, []() {        
        ulong currentTime = millis();
        lastInputRelayOnTime = currentTime;

        // if the last good cycle is not within the MIN_ON_TIME_WINDOW discard the current cycle
        if ((currentTime - lastInputRelayOffTime) < MIN_ON_TIME_WINDOW && (currentTime - lastGoodOffTime) > MIN_ON_TIME_WINDOW) {          
          timerId = simpleTimer.setTimeout(MIN_ON_TIME * 2, []() {turnOn();} );
        } else {
          timerId = simpleTimer.setTimeout(RELAY_ON_DELAY, []() {turnOn();} );
        }
      });
      
    } else {     
      simpleTimer.setTimeout(100, []() {
        ulong currentTime = millis();
        lastInputRelayOffTime = currentTime;
        
        // a "good" cycle has a minimum ON time
        bool isGoodCycle = (currentTime - lastInputRelayOnTime) >= MIN_ON_TIME;
        
        if (isGoodCycle) {
          lastGoodOffTime = currentTime;
          stats.lastGoodOffTime = lastGoodOffTime;
        }        
      });

      timerId = simpleTimer.setTimeout(RELAY_OFF_DELAY, turnOff);
    }
  
    // move this outside of ISR
    simpleTimer.setTimeout(50, onInputRelayChanged); 
  }
}

// ------------------------------------------------------------------------------------------

void onInputRelayChanged() {
  if (isInputRelayOn()) {
    TelnetStream.println("Input relay on");      
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    TelnetStream.println("Input relay off");      
    digitalWrite(LED_BUILTIN, HIGH);   
  }

  publishInputRelayStatus();
}

// ------------------------------------------------------------------------------------------

void publishOutputRelayStatus() {
  if (isBoilerOn()) {
    TelnetStream.println("Output relay on");
    mqttPublish(outputRelayTopic, "{\"state\": \"ON\"}");
  } else {
    TelnetStream.println("Output relay off");
    mqttPublish(outputRelayTopic, "{\"state\": \"OFF\"}");    
  }
}

// ------------------------------------------------------------------------------------------

void ensureMqttConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    return;      
  }
  
  if (mqtt.connected()) {    
    return;
  }
  
  TelnetStream.print("Connecting to the mqtt broker... ");
  
  if (mqtt.connect(HOSTNAME, DEFAULT_MQTT_USERNAME, DEFAULT_MQTT_PASSWORD, availabilityTopic, 0, true, "{\"status\": \"offline\"}"), true) {
    TelnetStream.print("connected");    
    mqttPublish(availabilityTopic, "{\"status\": \"online\"}", true);
    mqtt.subscribe("saboiot/mierebine/actuator");
    mqtt.subscribe("saboiot/mierebine/boiler");
    publishInputRelayStatus();
    publishOutputRelayStatus();
  } else {    
    TelnetStream.print("failed with state ");
    TelnetStream.print(mqtt.state()); 
  }
  TelnetStream.println("");
}

// ------------------------------------------------------------------------------------------

void onTimeUpdated() {
  time_t tnow = time(nullptr);
  if (!stats.timeStarted) {
    stats.timeStarted = tnow;
  }
}

// ------------------------------------------------------------------------------------------

void setupOTA() {
  ArduinoOTA.begin();
  ArduinoOTA.onStart([]() {
    Serial.println("OTA firmware update started");
    hwTimer.disableTimer();
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA Updated completed");
    hwTimer.enableTimer();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    hwTimer.enableTimer();
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {  
  char message[100];
  strncpy(message, (char*)payload, length);
  message[length] = '\0';

  TelnetStream.print("Message arrived on topic:");
  TelnetStream.println(topic);

  if (String(topic) == "saboiot/mierebine/boiler") {
    TelnetStream.print("Boiler:");
    TelnetStream.println(message);    
    
    if (String(message) == "on") {
      hasExternalHeatDemand = true;
      turnOn(1);
    } else {
      hasExternalHeatDemand = false;
      if (!hasInternalHeatDemand) {
        turnOff();          
      }
    }
  } else if (String(topic) == "saboiot/mierebine/actuator") {
    TelnetStream.print("Actuator:");
    TelnetStream.println(message);

    String(message) == "on" ? openActuator() : closeActuator();
  }
   
}

// ------------------------------------------------------------------------------------------

void setup() {    
  pinMode(BOILER_RELAY_PIN, OUTPUT);    
  pinMode(ACTUATOR_RELAY_PIN, OUTPUT);    
  pinMode(LED_BUILTIN, OUTPUT);    
  digitalWrite(LED_BUILTIN, HIGH); // turn off
  digitalWrite(BOILER_RELAY_PIN, HIGH); // turn off
  digitalWrite(ACTUATOR_RELAY_PIN, HIGH); // turn off  
  inputRelay.attach(HCE80_RELAY_INPUT_PIN, INPUT_PULLUP);
  inputRelay.interval(INPUT_RELAY_DEBOUNCE);
  pinMode(CONFIG_PORTAL_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP  
  wifi_set_sleep_type(LIGHT_SLEEP_T);
  
  Serial.begin(115200);

  ArduinoOTA.setHostname(HOSTNAME);
  
  wifiManager.setHostname(HOSTNAME);
  wifiManager.setWiFiAutoReconnect(true);
  wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_SEC);
  wifiManager.setEnableConfigPortal(false);
  wifiManager.setConfigPortalBlocking(true);
  wifiManager.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SEC);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.autoConnect();

  TelnetStream.begin();

  // must be before logging starts
  configTime(TIME_TZ, TIME_NTP_SERVER); 
  settimeofday_cb(onTimeUpdated);

  MDNS.begin(HOSTNAME);

  mqtt.setServer(mqttBroker, 1883);
  mqtt.setKeepAlive(MQTT_KEEP_ALIVE_SEC);
  mqtt.setCallback(mqttCallback);  
  ensureMqttConnected();

  // the hardware timer is used to safely capture the input relay changes  
  // hardware timer interval is expressed in nano seconds, but constant is in ms
  // leave this interrupt setup after wifi and mqtt
  hwTimer.attachInterruptInterval(HW_TIMER_INTERVAL * 1000, hwTimerHandler); 
  lastGoodOffTime = millis();
  stats.lastGoodOffTime = lastGoodOffTime;

  if (isInputRelayOn()) {
    turnOn();
  }
  
  simpleTimer.setInterval(MQTT_WATCHDOG_INTERVAL, ensureMqttConnected);
  simpleTimer.setInterval(MQTT_HANDLING_INTERVAL, []() {mqtt.loop();});
  
  setupOTA();
}

// ------------------------------------------------------------------------------------------

void doWiFiManager(){
  // is configuration portal requested?
  if (digitalRead(CONFIG_PORTAL_PIN) == LOW) {
    Serial.println("Starting Config Portal");
    // important - disable the hardware timer to avoid a brownout after save
    hwTimer.disableTimer();
    wifiManager.setEnableConfigPortal(true);
    wifiManager.startConfigPortal(CONFIG_PORTAL_AP_NAME, CONFIG_PORTAL_AP_PASS);
    wifiManager.setEnableConfigPortal(false);    
    hwTimer.enableTimer(); 
  }
}

// ------------------------------------------------------------------------------------------

void saveConfigCallback()
{
  Serial.println("Params saved");     
}

// ------------------------------------------------------------------------------------------

void loop() {    
  simpleTimer.run();  
  ArduinoOTA.handle();  
  MDNS.update();
  //doWiFiManager();
}
