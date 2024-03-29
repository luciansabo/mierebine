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
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

// logging related includes
#include <time.h>
#include <coredecls.h>                  // settimeofday_cb()
#include <CircularBuffer.h> // https://github.com/rlogiacco/CircularBuffer
#include <ESP8266WebServer.h>
#include "logging.h"

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

// logging
ESP8266WebServer server(80);
CircularBuffer<logRecord,300> logs;

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

void LOG(EventCode code) {
  time_t tnow = time(nullptr);  
  logs.push(logRecord{tnow, code});
}

// ------------------------------------------------------------------------------------------

String formatRow(logRecord logRow) {
  char buf[sizeof "fri 20 Jan 20:00:00"];
  time_t now = time(&now);  
  strftime(buf, sizeof buf, "%a %e %b %T", localtime(&logRow.time));
  
  String text = (String)buf + ",";
      
  switch (logRow.code) {
    case deviceOn:
      text += "deviceOn";
      break;
    case inputRelayOn:
      text += "inputRelayOn";
      break;
    case inputRelayOff:
      text += "inputRelayOff";
      break;
    case outputRelayOn:
      text += "outputRelayOn";
      break;
    case outputRelayOff:
      text += "outputRelayOff";
      break;      
    case mqttDisconnected:
      text += "mqttDisconnected";
      break;     
    case mqttConnected:
      text += "mqttConnected";
      break;    
    case mqttConnectFrror:
      text += "mqttConnectFrror";
      break;     
    case mqttPublishError:
      text += "mqttPublishError";
      break;     
    case maxRuntimeProtection:
      text += "maxRuntimeProtection";
      break;    
    case shortCycleDetected:
      text += "shortCycleDetected";
      break;  
    default:
      text += "unknown";
      break;     
  }

  text += "\n";

  return text;
}

// ------------------------------------------------------------------------------------------

bool isBoilerOn() {  
  return !digitalRead(BOILER_RELAY_PIN);  
}

// ------------------------------------------------------------------------------------------

void mqttPublish(const char *topic, const char *message, bool retain = false) {  
  ensureMqttConnected();

  if (!mqtt.publish(topic, message, retain)) {
    LOG(EventCode::mqttPublishError);
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
  LOG(EventCode::maxRuntimeProtection);
}
// ------------------------------------------------------------------------------------------

void turnOn() {  
  digitalWrite(BOILER_RELAY_PIN, LOW); // turn boiler relay on    
  //maxOnTimeTimer = simpleTimer.setTimeout(MAX_ON_TIME, onRuntimeProtection);  
  LOG(EventCode::outputRelayOn);
  
  publishOutputRelayStatus();
}  

// ------------------------------------------------------------------------------------------

void turnOff() {
  //simpleTimer.deleteTimer(maxOnTimeTimer);  
   
  if (!isBoilerOn()) {
    return;
  }
  
  digitalWrite(BOILER_RELAY_PIN, HIGH); // turn boiler relay off      
  LOG(EventCode::outputRelayOff);
      
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
      simpleTimer.setTimeout(100, [&lastInputRelayOnTime, &timerId]() {        
        ulong currentTime = millis();
        lastInputRelayOnTime = currentTime;

        // if the last good cycle is not within the MIN_ON_TIME_WINDOW discard the current cycle
        if ((currentTime - lastInputRelayOffTime) < MIN_ON_TIME_WINDOW && (currentTime - lastGoodOffTime) > MIN_ON_TIME_WINDOW) {
          LOG(EventCode::shortCycleDetected);
          timerId = simpleTimer.setTimeout(MIN_ON_TIME * 2, turnOn);          
        } else {
          timerId = simpleTimer.setTimeout(RELAY_ON_DELAY, turnOn);
        }
      });
      
    } else {     
      simpleTimer.setTimeout(100, [&lastInputRelayOffTime, &lastGoodOffTime, &stats]() {
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
    Serial.println("Input relay on");  
    LOG(EventCode::inputRelayOn);
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    Serial.println("Input relay off");  
    LOG(EventCode::inputRelayOff);
    digitalWrite(LED_BUILTIN, HIGH);   
  }

  publishInputRelayStatus();
}

// ------------------------------------------------------------------------------------------

void publishOutputRelayStatus() {
  if (isBoilerOn()) {
    Serial.println("Output relay on");
    mqttPublish(outputRelayTopic, "{\"state\": \"ON\"}");
  } else {
    Serial.println("Output relay off");
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

  LOG(EventCode::mqttDisconnected);
  
  Serial.print("Connecting to the mqtt broker... ");
  
  if (!mqtt.connect(HOSTNAME, DEFAULT_MQTT_USERNAME, DEFAULT_MQTT_PASSWORD, availabilityTopic, 0, true, "{\"status\": \"offline\"}"), true) {
    Serial.print("connected");
    LOG(EventCode::mqttConnected);
    mqttPublish(availabilityTopic, "{\"status\": \"online\"}", true);
    publishInputRelayStatus();
    publishOutputRelayStatus();
  } else {
    LOG(EventCode::mqttConnectFrror);
    Serial.print("failed with state ");
    Serial.print(mqtt.state()); 
  }
  Serial.println("");
}

// ------------------------------------------------------------------------------------------

void setupWebserver() {
  MDNS.begin(HOSTNAME);
  
  // webserver for logging

  server.on("/logs", []() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send ( 200, "text/plain", "");
    for (int i = 0; i < logs.size(); i++) {      
      server.sendContent(formatRow(logs[i]));      
    }
  });

  server.on("/status", []() {    
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send (200, "text/plain", "");
    server.sendContent("Started (GMT): " + (String)ctime(&stats.timeStarted));
    server.sendContent("Input relay: " + (String)(isInputRelayOn() ? "on" : "off") + "\n");
    server.sendContent("Output relay: " + (String)(isBoilerOn() ? "on" : "off") + "\n");
    server.sendContent("Last good cycle off time: " +
      (String)((String)((millis() - stats.lastGoodOffTime) / 60000) + " minutes ago"));
  });

  server.begin();
  Serial.println("Webserver started");
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

// ------------------------------------------------------------------------------------------

void setup() {    
  pinMode(BOILER_RELAY_PIN, OUTPUT);    
  pinMode(LED_BUILTIN, OUTPUT);    
  digitalWrite(LED_BUILTIN, HIGH); // turn off
  digitalWrite(BOILER_RELAY_PIN, HIGH); // turn off
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

  // must be before logging starts
  configTime(TIME_TZ, TIME_NTP_SERVER); 
  settimeofday_cb(onTimeUpdated);

  LOG(EventCode::deviceOn);

  mqtt.setServer(mqttBroker, 1883);
  mqtt.setKeepAlive(MQTT_KEEP_ALIVE_SEC);
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
  
  setupWebserver();
  
  setupOTA();
}

// ------------------------------------------------------------------------------------------

void doWiFiManager(){
  // is configuration portal requested?
  if (digitalRead(CONFIG_PORTAL_PIN) == LOW) {
    Serial.println("Starting Config Portal");
    // important - disable webserver to be able to use the config portal routes
    server.stop();    
    // important - disable the hardware timer to avoid a brownout after save
    hwTimer.disableTimer();
    wifiManager.setEnableConfigPortal(true);
    wifiManager.startConfigPortal(CONFIG_PORTAL_AP_NAME, CONFIG_PORTAL_AP_PASS);
    wifiManager.setEnableConfigPortal(false);    
    hwTimer.enableTimer();
    server.begin();    
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
  server.handleClient();
  MDNS.update();
  doWiFiManager();
}
