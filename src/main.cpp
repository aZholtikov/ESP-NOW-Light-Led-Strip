#include "ArduinoJson.h"
#include "ArduinoOTA.h"
#include "ESPAsyncWebServer.h"
#include "Ticker.h"
#include "ZHNetwork.h"
#include "ZHConfig.h"

void onBroadcastReceiving(const char *data, const uint8_t *sender);
void onUnicastReceiving(const char *data, const uint8_t *sender);
void onConfirmReceiving(const uint8_t *target, const bool status);

void loadConfig(void);
void saveConfig(void);
void setupWebServer(void);

void sendAttributesMessage(void);
void sendKeepAliveMessage(void);
void sendConfigMessage(void);
void sendStatusMessage(void);

String getValue(String data, char separator, byte index);

void changeLedState(void);

const String firmware{"1.11"};

String espnowNetName{"DEFAULT"};

String deviceName = "ESP-NOW light " + String(ESP.getChipId(), HEX);

uint8_t ledType{ENLT_NONE};
bool ledStatus{false};
uint8_t coldWhitePin{0};
uint8_t warmWhitePin{0};
uint8_t redPin{0};
uint8_t greenPin{0};
uint8_t bluePin{0};

uint8_t brightness{255};
uint16_t temperature{255};
uint8_t red{255};
uint8_t green{255};
uint8_t blue{255};

bool wasMqttAvailable{false};

uint8_t gatewayMAC[6]{0};

const String payloadOn{"ON"};
const String payloadOff{"OFF"};

ZHNetwork myNet;
AsyncWebServer webServer(80);

Ticker gatewayAvailabilityCheckTimer;
bool isGatewayAvailable{false};
void gatewayAvailabilityCheckTimerCallback(void);

Ticker apModeHideTimer;
void apModeHideTimerCallback(void);

Ticker attributesMessageTimer;
bool attributesMessageTimerSemaphore{true};
void attributesMessageTimerCallback(void);
Ticker attributesMessageResendTimer;
bool attributesMessageResendTimerSemaphore{false};

Ticker keepAliveMessageTimer;
bool keepAliveMessageTimerSemaphore{true};
void keepAliveMessageTimerCallback(void);
Ticker keepAliveMessageResendTimer;
bool keepAliveMessageResendTimerSemaphore{false};

Ticker configMessageResendTimer;
bool configMessageResendTimerSemaphore{false};

Ticker statusMessageTimer;
bool statusMessageTimerSemaphore{true};
void statusMessageTimerCallback(void);
Ticker statusMessageResendTimer;
bool statusMessageResendTimerSemaphore{false};

void setup()
{
  analogWriteRange(255);

  SPIFFS.begin();

  loadConfig();

  if (coldWhitePin)
    pinMode(coldWhitePin, OUTPUT);
  if (warmWhitePin)
    pinMode(warmWhitePin, OUTPUT);
  if (redPin)
    pinMode(redPin, OUTPUT);
  if (greenPin)
    pinMode(greenPin, OUTPUT);
  if (bluePin)
    pinMode(bluePin, OUTPUT);

  changeLedState();

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  myNet.begin(espnowNetName.c_str());

  myNet.setOnBroadcastReceivingCallback(onBroadcastReceiving);
  myNet.setOnUnicastReceivingCallback(onUnicastReceiving);
  myNet.setOnConfirmReceivingCallback(onConfirmReceiving);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(("ESP-NOW light " + String(ESP.getChipId(), HEX)).c_str(), "12345678", 1, 0);
  apModeHideTimer.once(300, apModeHideTimerCallback);

  setupWebServer();

  ArduinoOTA.begin();

  attributesMessageTimer.attach(60, attributesMessageTimerCallback);
  keepAliveMessageTimer.attach(10, keepAliveMessageTimerCallback);
  statusMessageTimer.attach(300, statusMessageTimerCallback);
}

void loop()
{
  if (attributesMessageTimerSemaphore)
    sendAttributesMessage();
  if (keepAliveMessageTimerSemaphore)
    sendKeepAliveMessage();
  if (statusMessageTimerSemaphore)
    sendStatusMessage();
  myNet.maintenance();
  ArduinoOTA.handle();
}

void onBroadcastReceiving(const char *data, const byte *sender)
{
  esp_now_payload_data_t incomingData;
  memcpy(&incomingData, data, sizeof(esp_now_payload_data_t));
  if (incomingData.deviceType != ENDT_GATEWAY)
    return;
  if (myNet.macToString(gatewayMAC) != myNet.macToString(sender) && incomingData.payloadsType == ENPT_KEEP_ALIVE)
    memcpy(gatewayMAC, sender, 6);
  if (myNet.macToString(gatewayMAC) == myNet.macToString(sender) && incomingData.payloadsType == ENPT_KEEP_ALIVE)
  {
    isGatewayAvailable = true;
    StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
    deserializeJson(json, incomingData.message);
    bool temp = json["MQTT"] == "online" ? true : false;
    if (wasMqttAvailable != temp)
    {
      wasMqttAvailable = temp;
      if (temp)
        sendConfigMessage();
    }
    gatewayAvailabilityCheckTimer.once(15, gatewayAvailabilityCheckTimerCallback);
  }
}

void onUnicastReceiving(const char *data, const byte *sender)
{
  esp_now_payload_data_t incomingData;
  memcpy(&incomingData, data, sizeof(esp_now_payload_data_t));
  if (incomingData.deviceType != ENDT_GATEWAY || myNet.macToString(gatewayMAC) != myNet.macToString(sender))
    return;
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  if (incomingData.payloadsType == ENPT_SET)
  {
    deserializeJson(json, incomingData.message);
    if (json["set"])
      ledStatus = json["set"] == payloadOn ? true : false;
    if (json["brightness"])
      brightness = json["brightness"];
    if (json["temperature"])
      temperature = json["temperature"];
    if (json["rgb"])
    {
      red = getValue(String(json["rgb"].as<String>()).substring(0, sizeof(esp_now_payload_data_t::message)).c_str(), ',', 0).toInt();
      green = getValue(String(json["rgb"].as<String>()).substring(0, sizeof(esp_now_payload_data_t::message)).c_str(), ',', 1).toInt();
      blue = getValue(String(json["rgb"].as<String>()).substring(0, sizeof(esp_now_payload_data_t::message)).c_str(), ',', 2).toInt();
    }
    changeLedState();
    sendStatusMessage();
  }
  if (incomingData.payloadsType == ENPT_UPDATE)
  {
    WiFi.softAP(("ESP-NOW light " + String(ESP.getChipId(), HEX)).c_str(), "12345678", 1, 0);
    webServer.begin();
    apModeHideTimer.once(300, apModeHideTimerCallback);
  }
  if (incomingData.payloadsType == ENPT_RESTART)
    ESP.restart();
}

void onConfirmReceiving(const uint8_t *target, const bool status)
{
  if (status)
  {
    if (attributesMessageResendTimerSemaphore)
    {
      attributesMessageResendTimerSemaphore = false;
      attributesMessageResendTimer.detach();
    }
    if (keepAliveMessageResendTimerSemaphore)
    {
      keepAliveMessageResendTimerSemaphore = false;
      keepAliveMessageResendTimer.detach();
    }
    if (configMessageResendTimerSemaphore)
    {
      configMessageResendTimerSemaphore = false;
      configMessageResendTimer.detach();
    }
    if (statusMessageResendTimerSemaphore)
    {
      statusMessageResendTimerSemaphore = false;
      statusMessageResendTimer.detach();
    }
  }
}

void loadConfig()
{
  if (!SPIFFS.exists("/config.json"))
    saveConfig();
  File file = SPIFFS.open("/config.json", "r");
  String jsonFile = file.readString();
  StaticJsonDocument<512> json;
  deserializeJson(json, jsonFile);
  espnowNetName = json["espnowNetName"].as<String>();
  deviceName = json["deviceName"].as<String>();
  ledType = json["ledType"];
  ledStatus = json["ledStatus"];
  coldWhitePin = json["coldWhitePin"];
  warmWhitePin = json["warmWhitePin"];
  redPin = json["redPin"];
  greenPin = json["greenPin"];
  bluePin = json["bluePin"];
  brightness = json["brightness"];
  temperature = json["temperature"];
  red = json["red"];
  green = json["green"];
  blue = json["blue"];
  file.close();
}

void saveConfig()
{
  StaticJsonDocument<512> json;
  json["firmware"] = firmware;
  json["espnowNetName"] = espnowNetName;
  json["deviceName"] = deviceName;
  json["ledType"] = ledType;
  json["ledStatus"] = ledStatus;
  json["coldWhitePin"] = coldWhitePin;
  json["warmWhitePin"] = warmWhitePin;
  json["redPin"] = redPin;
  json["greenPin"] = greenPin;
  json["bluePin"] = bluePin;
  json["brightness"] = brightness;
  json["temperature"] = temperature;
  json["red"] = red;
  json["green"] = green;
  json["blue"] = blue;
  json["system"] = "empty";
  File file = SPIFFS.open("/config.json", "w");
  serializeJsonPretty(json, file);
  file.close();
}

void setupWebServer()
{
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(SPIFFS, "/index.htm"); });

  webServer.on("/setting", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        ledType = request->getParam("ledType")->value().toInt();
        coldWhitePin = request->getParam("coldWhitePin")->value().toInt();
        warmWhitePin = request->getParam("warmWhitePin")->value().toInt();
        redPin = request->getParam("redPin")->value().toInt();
        greenPin = request->getParam("greenPin")->value().toInt();
        bluePin = request->getParam("bluePin")->value().toInt();
        deviceName = request->getParam("deviceName")->value();
        espnowNetName = request->getParam("espnowNetName")->value();
        request->send(200);
        saveConfig(); });

  webServer.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        request->send(200);
        ESP.restart(); });

  webServer.onNotFound([](AsyncWebServerRequest *request)
                       { 
        if (SPIFFS.exists(request->url()))
        request->send(SPIFFS, request->url());
        else
        {
        request->send(404, "text/plain", "File Not Found");
        } });

  webServer.begin();
}

void sendAttributesMessage()
{
  if (!isGatewayAvailable)
    return;
  attributesMessageTimerSemaphore = false;
  uint32_t secs = millis() / 1000;
  uint32_t mins = secs / 60;
  uint32_t hours = mins / 60;
  uint32_t days = hours / 24;
  esp_now_payload_data_t outgoingData{ENDT_LED, ENPT_ATTRIBUTES};
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  json["Type"] = "ESP-NOW light";
  json["MCU"] = "ESP8266";
  json["MAC"] = myNet.getNodeMac();
  json["Firmware"] = firmware;
  json["Library"] = myNet.getFirmwareVersion();
  json["Uptime"] = "Days:" + String(days) + " Hours:" + String(hours - (days * 24)) + " Mins:" + String(mins - (hours * 60));
  char buffer[sizeof(esp_now_payload_data_t::message)]{0};
  serializeJsonPretty(json, buffer);
  memcpy(outgoingData.message, buffer, sizeof(esp_now_payload_data_t::message));
  char temp[sizeof(esp_now_payload_data_t)]{0};
  memcpy(&temp, &outgoingData, sizeof(esp_now_payload_data_t));
  myNet.sendUnicastMessage(temp, gatewayMAC, true);

  attributesMessageResendTimerSemaphore = true;
  attributesMessageResendTimer.once(1, sendAttributesMessage);
}

void sendKeepAliveMessage()
{
  if (!isGatewayAvailable)
    return;
  keepAliveMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_LED, ENPT_KEEP_ALIVE};
  char temp[sizeof(esp_now_payload_data_t)]{0};
  memcpy(&temp, &outgoingData, sizeof(esp_now_payload_data_t));
  myNet.sendUnicastMessage(temp, gatewayMAC, true);

  keepAliveMessageResendTimerSemaphore = true;
  keepAliveMessageResendTimer.once(1, sendKeepAliveMessage);
}

void sendConfigMessage()
{
  if (!isGatewayAvailable)
    return;
  esp_now_payload_data_t outgoingData{ENDT_LED, ENPT_CONFIG};
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  json["name"] = deviceName;
  json["unit"] = 1;
  json["type"] = HACT_LIGHT;
  json["class"] = ledType;
  json["payload_on"] = payloadOn;
  json["payload_off"] = payloadOff;
  char buffer[sizeof(esp_now_payload_data_t::message)]{0};
  serializeJsonPretty(json, buffer);
  memcpy(outgoingData.message, buffer, sizeof(esp_now_payload_data_t::message));
  char temp[sizeof(esp_now_payload_data_t)]{0};
  memcpy(&temp, &outgoingData, sizeof(esp_now_payload_data_t));
  myNet.sendUnicastMessage(temp, gatewayMAC, true);

  configMessageResendTimerSemaphore = true;
  configMessageResendTimer.once(1, sendConfigMessage);
}

void sendStatusMessage()
{
  if (!isGatewayAvailable)
    return;
  statusMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_LED, ENPT_STATE};
  StaticJsonDocument<sizeof(esp_now_payload_data_t::message)> json;
  json["state"] = ledStatus ? payloadOn : payloadOff;
  json["brightness"] = brightness;
  json["temperature"] = temperature;
  json["rgb"] = String(red) + "," + String(green) + "," + String(blue);
  char buffer[sizeof(esp_now_payload_data_t::message)]{0};
  serializeJsonPretty(json, buffer);
  memcpy(&outgoingData.message, &buffer, sizeof(esp_now_payload_data_t::message));
  char temp[sizeof(esp_now_payload_data_t)]{0};
  memcpy(&temp, &outgoingData, sizeof(esp_now_payload_data_t));
  myNet.sendUnicastMessage(temp, gatewayMAC, true);

  statusMessageResendTimerSemaphore = true;
  statusMessageResendTimer.once(1, sendStatusMessage);
}

String getValue(String data, char separator, byte index)
{
  byte found{0};
  int strIndex[]{0, -1};
  int maxIndex = data.length() - 1;
  for (byte i{0}; i <= maxIndex && found <= index; i++)
    if (data.charAt(i) == separator || i == maxIndex)
    {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void changeLedState(void)
{
  if (ledStatus)
  {
    if (red == 255 && green == 255 && blue == 255)
    {
      if (ledType == ENLT_W || ledType == ENLT_RGBW)
        analogWrite(coldWhitePin, brightness);
      if (ledType == ENLT_WW || ledType == ENLT_RGBWW)
      {
        analogWrite(coldWhitePin, map(brightness, 0, 255, 0, map(temperature, 500, 153, 0, 255)));
        analogWrite(warmWhitePin, map(brightness, 0, 255, 0, map(temperature, 153, 500, 0, 255)));
      }
      if (ledType == ENLT_RGB)
      {
        analogWrite(redPin, map(red, 0, 255, 0, brightness));
        analogWrite(greenPin, map(green, 0, 255, 0, brightness));
        analogWrite(bluePin, map(blue, 0, 255, 0, brightness));
      }
      if (ledType == ENLT_RGBW || ledType == ENLT_RGBWW)
      {
        digitalWrite(redPin, LOW);
        digitalWrite(greenPin, LOW);
        digitalWrite(bluePin, LOW);
      }
    }
    else
    {
      if (ledType == ENLT_W)
        analogWrite(coldWhitePin, brightness);
      if (ledType == ENLT_WW)
      {
        analogWrite(coldWhitePin, map(brightness, 0, 255, 0, map(temperature, 500, 153, 0, 255)));
        analogWrite(warmWhitePin, map(brightness, 0, 255, 0, map(temperature, 153, 500, 0, 255)));
      }
      if (ledType == ENLT_RGBW || ledType == ENLT_RGBWW)
        digitalWrite(coldWhitePin, LOW);
      if (ledType == ENLT_RGBWW)
        digitalWrite(warmWhitePin, LOW);
      if (ledType == ENLT_RGB || ledType == ENLT_RGBW || ledType == ENLT_RGBWW)
      {
        analogWrite(redPin, map(red, 0, 255, 0, brightness));
        analogWrite(greenPin, map(green, 0, 255, 0, brightness));
        analogWrite(bluePin, map(blue, 0, 255, 0, brightness));
      }
    }
  }
  else
  {
    if (ledType == ENLT_W || ledType == ENLT_WW || ledType == ENLT_RGBW || ledType == ENLT_RGBWW)
      digitalWrite(coldWhitePin, LOW);
    if (ledType == ENLT_WW || ledType == ENLT_RGBWW)
      digitalWrite(warmWhitePin, LOW);
    if (ledType == ENLT_RGB || ledType == ENLT_RGBW || ledType == ENLT_RGBWW)
    {
      digitalWrite(redPin, LOW);
      digitalWrite(greenPin, LOW);
      digitalWrite(bluePin, LOW);
    }
  }
  saveConfig();
}

void gatewayAvailabilityCheckTimerCallback()
{
  isGatewayAvailable = false;
  memset(gatewayMAC, 0, 6);
}

void apModeHideTimerCallback()
{
  WiFi.softAP(("ESP-NOW light " + String(ESP.getChipId(), HEX)).c_str(), "12345678", 1, 1);
  webServer.end();
}

void attributesMessageTimerCallback()
{
  attributesMessageTimerSemaphore = true;
}

void keepAliveMessageTimerCallback()
{
  keepAliveMessageTimerSemaphore = true;
}

void statusMessageTimerCallback()
{
  statusMessageTimerSemaphore = true;
}