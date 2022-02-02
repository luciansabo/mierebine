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
  time_t time;
  EventCode code;    
} logRecord;
