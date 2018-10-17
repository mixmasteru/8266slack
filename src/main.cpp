#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>

#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "icon.h"

#include <GxEPD.h>
#include <GxGDEH029A1/GxGDEH029A1.cpp>      // 2.9" b/w
#include <GxIO/GxIO_SPI/GxIO_SPI.cpp>
#include <GxIO/GxIO.cpp>
// FreeFonts from Adafruit_GFX
#include <Fonts/FreeMonoBold9pt7b.h>

GxIO_Class io(SPI, /*CS=D8*/ SS, /*DC=D3*/ 0, /*RST=D4*/ 2); // arbitrary selection of D3(=0), D4(=2), selected for default of GxEPD_Class
GxEPD_Class display(io /*RST=D4*/ /*BUSY=D2*/); // default selection of D4(=2), D2(=4)

// (SHA-1) If Slack changes their SSL fingerprint, you would need to update this
#define SLACK_SSL_FINGERPRINT "C1 0D 53 49 D2 3E E5 2B A2 61 D5 9E 6F 99 0D 3D FD 8B B2 B3"

ESP8266WiFiMulti WiFiMulti;
WebSocketsClient webSocket;

long nextCmdId = 1;
bool connected = false;
unsigned long lastPing = 0;

const GFXfont* f = &FreeMonoBold9pt7b;
const char *last_message = "";


void showMessageCallback()
{
  display.setRotation(1);
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setFont(f);
  display.setCursor(0, 0);
  display.println();
  display.println(last_message);
}

void showAttachment(JsonObject& root)
{
  display.setRotation(1);
  display.fillScreen(GxEPD_WHITE);

  display.drawBitmap(aws_icon_bits,  0, 0, aws_icon_width, aws_icon_height, GxEPD_BLACK);
  display.update();

  display.setTextColor(GxEPD_BLACK);
  display.setFont(f);
  display.println();
  display.setCursor(aws_icon_width + 10, display.getCursorY());
  display.println(root["attachments"][0]["pretext"].as<char*>());
  display.setCursor(aws_icon_width + 10, display.getCursorY());
  display.println(root["attachments"][0]["title"].as<char*>());
  display.println();
  display.println(root["attachments"][0]["text"].as<char*>());
  display.update();
}

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
payload: {"text":"test SUCCEEDED","bot_id":"BDAEDGKCZ",
          "type":"message",
          "subtype":"bot_message",
          "team":"AAAAAAAA",
          "channel":"AAAAAA",
          "event_ts":"1539460264.000100",
          "ts":"1539460264.000100"}

          {"text":"","bot_id":"BDAEDGKCZ",
          "attachments":[{"fallback":"AWS codebuild: SampleProjectName IN_PROGRESS",
          "text":"build-status: IN_PROGRESS",
          "pretext":"CodeBuild Build State Change","title":"SampleProjectName","id":1,"title_link":"https:\/\/","ts":1499820148,"color":"36a64f"}],
          "type":"message","subtype":"bot_message","team":"TDB2CAKGA","channel":"GDAAHFD24","event_ts":"1539604493.000100","ts":"1539604493.000100"}
**/
void processSlackMessage(char *payload) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(payload);
  if (!root.success())
      Serial.println(F("Failed to read payload, using default configuration"));

  JsonVariant att = root["attachments"];
  if(att.success()){
    showAttachment(root);
  }
  else if(root["type"] == "message" && root["text"] != ""){
    last_message = root["text"];
    display.drawPaged(showMessageCallback);
  }
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
  display.init(115200);

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
