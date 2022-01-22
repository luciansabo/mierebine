/**
 * MiereBine (Pardosilla) v 2022.1
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

#include <WiFiManager.h> 
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <SimpleTimer.h>
#include <ArduinoOTA.h>
#include "config.h"
#include "config.local.h"

#include "logging.h"

// logging related includes
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <CircularBuffer.h>
#include <ESP8266WebServer.h>

volatile int timerId, maxOnTimeTimer;

WiFiClient espClient;
PubSubClient mqtt(espClient);
SimpleTimer simpleTimer;

// Wifimanager variables
WiFiManager wifiManager;
bool portalRunning      = false;
uint configPortalStartTime = millis();

// time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

// logging
ESP8266WebServer server(80);
CircularBuffer<logRecord,300> logs;

// ------------------------------------------------------------------------------------------

void LOG(EventCode code) {
   logs.push(logRecord{timeClient.getEpochTime(), code});
}

// ------------------------------------------------------------------------------------------

String formatRow(logRecord logRow) {
  String text = (String)logRow.time + ",";
      
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
  return !digitalRead(HCE80_RELAY_INPUT_PIN);
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

ICACHE_RAM_ATTR void onRelayStatusChanged() {  
  // remove the old timer to avoid triggering on/off
  // if the source relay toggled before the timer interval  
  simpleTimer.deleteTimer(timerId);
  
  if (isInputRelayOn()) {        
    timerId = simpleTimer.setTimeout(RELAY_DELAY, turnOn);
    LOG(EventCode::inputRelayOn);
    digitalWrite(LED_BUILTIN, LOW);
  } else {        
    timerId = simpleTimer.setTimeout(RELAY_DELAY, turnOff);
    LOG(EventCode::inputRelayOff);
    digitalWrite(LED_BUILTIN, HIGH);
  }

  // move this outside of ISR
  simpleTimer.setTimeout(10, publishInputRelayStatus);  
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

void setup() {    
  pinMode(BOILER_RELAY_PIN, OUTPUT);    
  pinMode(LED_BUILTIN, OUTPUT);    
  digitalWrite(LED_BUILTIN, HIGH); // turn off
  digitalWrite(BOILER_RELAY_PIN, HIGH); // turn off
  pinMode(HCE80_RELAY_INPUT_PIN, INPUT_PULLUP);
  pinMode(CONFIG_PORTAL_PIN, INPUT_PULLUP);  
  
  attachInterrupt(digitalPinToInterrupt(HCE80_RELAY_INPUT_PIN), onRelayStatusChanged, CHANGE);    

  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP  
  wifi_set_sleep_type(LIGHT_SLEEP_T);
  
  Serial.begin(115200);

  ArduinoOTA.setHostname(HOSTNAME);
  
  wifiManager.setHostname(HOSTNAME);
  wifiManager.setWiFiAutoReconnect(true);
  wifiManager.setConnectTimeout(30);
  wifiManager.setEnableConfigPortal(false);
  wifiManager.autoConnect();

  // must be before logging starts
  timeClient.begin();

  LOG(EventCode::deviceOn);

  mqtt.setServer(mqttBroker, mqttPort);
  mqtt.setKeepAlive(MQTT_KEEP_ALIVE_SEC);
  ensureMqttConnected();
  
  if (isInputRelayOn()) {
    turnOn();
  }
  simpleTimer.setInterval(MQTT_WATCHDOG_INTERVAL, ensureMqttConnected);
  simpleTimer.setInterval(MQTT_HANDLING_INTERVAL, []() {mqtt.loop();});
  
  setupWebserver();
  
  ArduinoOTA.begin();
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
  timeClient.update();  
}
