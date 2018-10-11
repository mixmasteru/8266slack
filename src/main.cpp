#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>

#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#include "secrets.h"

// (SHA-1) If Slack changes their SSL fingerprint, you would need to update this
#define SLACK_SSL_FINGERPRINT "C1 0D 53 49 D2 3E E5 2B A2 61 D5 9E 6F 99 0D 3D FD 8B B2 B3"
// Get token by creating new bot integration at https://my.slack.com/services/new/bot

ESP8266WiFiMulti WiFiMulti;
WebSocketsClient webSocket;

long nextCmdId = 1;
bool connected = false;
unsigned long lastPing = 0;

/**
  Sends a ping message to Slack. Call this function immediately after establishing
  the WebSocket connection, and then every 5 seconds to keep the connection alive.
*/
void sendPing() {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["type"] = "ping";
  root["id"] = nextCmdId++;
  String json;
  root.printTo(json);
  webSocket.sendTXT(json);
}

/**
  Looks for color names in the incoming slack messages and
  animates the ring accordingly. You can include several
  colors in a single message, e.g. `red blue zebra black yellow rainbow`
*/
void processSlackMessage(char *payload) {
  Serial.printf(payload);
}

/**
  Called on each web socket event. Handles disconnection, and also
  incoming messages from slack.
*/
void webSocketEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WebSocket] Disconnected :-( \n");
      connected = false;
      break;

    case WStype_CONNECTED:
      Serial.printf("[WebSocket] Connected to: %s\n", payload);
      sendPing();
      break;

    case WStype_TEXT:
      Serial.printf("[WebSocket] Message: %s\n", payload);
      processSlackMessage((char*)payload);
      break;
  }
}

/**
  Establishes a bot connection to Slack:
  1. Performs a REST call to get the WebSocket URL
  2. Conencts the WebSocket
  Returns true if the connection was established successfully.
*/
bool connectToSlack() {
  // Step 1: Find WebSocket address via RTM API (https://api.slack.com/methods/rtm.connect)
  HTTPClient http;
  http.begin("https://slack.com/api/rtm.connect?token=" SLACK_BOT_TOKEN, SLACK_SSL_FINGERPRINT);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed with code %d\n", httpCode);
    return false;
  }

  WiFiClient *client = http.getStreamPtr();
  client->find("wss:\\/\\/");
  String host = client->readStringUntil('\\');
  String path = client->readStringUntil('"');
  path.replace("\\/", "/");

  // Step 2: Open WebSocket connection and register event handler
  Serial.println("WebSocket Host=" + host + " Path=" + path);
  webSocket.beginSSL(host, 443, path, "", "");
  webSocket.onEvent(webSocketEvent);
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  WiFiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  while (WiFiMulti.run() != WL_CONNECTED) {
    delay(100);
  }

  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}

/**
  Sends a ping every 5 seconds, and handles reconnections
*/
void loop() {
  webSocket.loop();

  if (connected) {
    // Send ping every 5 seconds, to keep the connection alive
    if (millis() - lastPing > 5000) {
      sendPing();
      lastPing = millis();
    }
  } else {
    // Try to connect / reconnect to slack
    connected = connectToSlack();
    if (!connected) {
      delay(500);
    }
  }
}
