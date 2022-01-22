enum EventCode {
  deviceOn,
  inputRelayOn, 
  inputRelayOff,
  outputRelayOn,
  outputRelayOff,
  mqttDisconnected,
  mqttConnected,
  mqttConnectFrror,
  mqttPublishError,
  maxRuntimeProtection
};

typedef struct {
  unsigned long time;
  EventCode code;    
} logRecord;
