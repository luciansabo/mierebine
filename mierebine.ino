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
#include <CircularBuffer.h> // https://github.com/rlogiacco/CircularBuffer
#include <ESP8266WebServer.h>

#include "config.h"
#include "config.local.h"

#include "logging.h"

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
uint configPortalStartTime = millis();

// logging
ESP8266WebServer server(80);
CircularBuffer<logRecord,300> logs;

struct TStats {
  time_t timeStarted;
  unsigned int runs = 0;
} stats;

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

void mqttPublish(const char *topic, const char *message) {  
  ensureMqttConnected();

  if (!mqtt.publish(topic, message)) {
    LOG(EventCode::mqttPublishError);
    Serial.print("MQTT Publish failed: " );
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
  //lastOnTime = millis();
  LOG(EventCode::outputRelayOn);
  publishOutputRelayStatus();
  stats.runs++; 
}  

// ------------------------------------------------------------------------------------------

void turnOff() {
  //simpleTimer.deleteTimer(maxOnTimeTimer);  
   
  if (!isBoilerOn()) {
    return;
  }
  
  digitalWrite(BOILER_RELAY_PIN, HIGH); // turn boiler relay off    
  //lastOffTime = millis();
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
      timerId = simpleTimer.setTimeout(RELAY_DELAY, turnOn);
      LOG(EventCode::inputRelayOn);
    } else {        
      timerId = simpleTimer.setTimeout(RELAY_DELAY, turnOff);
      LOG(EventCode::inputRelayOff);
    }
  
    // move this outside of ISR
    simpleTimer.setTimeout(50, onInputRelayChanged); 
  }
}

// ------------------------------------------------------------------------------------------

void onInputRelayChanged() {
  if (isInputRelayOn()) {
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    digitalWrite(LED_BUILTIN, HIGH);   
  }

  publishInputRelayStatus();
}

// ------------------------------------------------------------------------------------------

void publishOutputRelayStatus() {
  if (isBoilerOn()) {
    mqttPublish(outputRelayTopic, "{\"state\": \"ON\"}");
  } else {
    mqttPublish(outputRelayTopic, "{\"state\": \"OFF\"}");    
  }
}

// ------------------------------------------------------------------------------------------

void ensureMqttConnected() {    
  if (mqtt.connected()) {    
    return;
  }

  LOG(EventCode::mqttDisconnected);
  
  Serial.print("Connecting to the mqtt broker... ");
  
  if (!mqtt.connect(HOSTNAME, mqttUsername, mqttPassword, availabilityTopic, 0, true, "{\"status\": \"offline\"}"), true) {
    Serial.print("connected");
    LOG(EventCode::mqttConnected);
    mqttPublish(availabilityTopic, "{\"status\": \"online\"}");
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

  server.begin();
  Serial.println("Webserver started");
}

// ------------------------------------------------------------------------------------------

/*void onTimeUpdated() {
  time_t tnow = time();
  if (!stats.timeStarted) {
    stats.timeStarted = tnow;
  }
}*/

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
  wifiManager.setConnectTimeout(30);
  //wifiManager.setEnableConfigPortal(false);
  wifiManager.autoConnect();

  // must be before logging starts
  configTime(TIME_TZ, TIME_NTP_SERVER); 
  //settimeofday_cb(onTimeUpdated);

  LOG(EventCode::deviceOn);

  mqtt.setServer(mqttBroker, mqttPort);
  mqtt.setKeepAlive(MQTT_KEEP_ALIVE_SEC);
  ensureMqttConnected();

  // the hardware timer is used to safely capture the input relay changes  
  // hardware timer interval is expressed in nano seconds, but constant is in ms
  // leave this interrupt setup after wifi and mqtt
  hwTimer.attachInterruptInterval(HW_TIMER_INTERVAL * 1000, hwTimerHandler); 

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
  // is auto timeout portal running
  if (portalRunning){
    wifiManager.process(); // do processing

    // check for timeout
    if ((millis() - configPortalStartTime) > (CONFIG_PORTAL_TIMEOUT)){
      Serial.println("Portal timeout");
      portalRunning = false;      
      wifiManager.stopConfigPortal();      
   }
  }

  // is configuration portal requested?
  if (digitalRead(CONFIG_PORTAL_PIN) == LOW && (!portalRunning)) {
    
    Serial.println("Starting Config Portal");
    wifiManager.setConfigPortalBlocking(false);
    wifiManager.startConfigPortal(CONFIG_PORTAL_AP_NAME, CONFIG_PORTAL_AP_PASS);
          
    portalRunning = true;
    configPortalStartTime = millis();
  }
}

// ------------------------------------------------------------------------------------------

void loop() {      
  simpleTimer.run();
  doWiFiManager();
  ArduinoOTA.handle();
  server.handleClient();
  MDNS.update();  
}
