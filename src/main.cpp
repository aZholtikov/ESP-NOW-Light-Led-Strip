#include "ArduinoJson.h"
#include "ArduinoOTA.h"
#include "ESPAsyncWebServer.h" // https://github.com/aZholtikov/Async-Web-Server
#include "LittleFS.h"
#include "EEPROM.h"
#include "Ticker.h"
#include "ZHNetwork.h"
#include "ZHConfig.h"

void onBroadcastReceiving(const char *data, const uint8_t *sender);
void onUnicastReceiving(const char *data, const uint8_t *sender);
void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status);

void loadConfig(void);
void saveConfig(void);
void loadStatus(void);
void saveStatus(void);
void setupWebServer(void);

void sendAttributesMessage(void);
void sendKeepAliveMessage(void);
void sendConfigMessage(void);
void sendStatusMessage(void);

String getValue(String data, char separator, uint8_t index);

void changeLedState(void);

typedef struct
{
  uint16_t id{0};
  char message[200]{0};
} espnow_message_t;

struct deviceConfig
{
  const String firmware{"1.21"};
  String espnowNetName{"DEFAULT"};
  String deviceName = "ESP-NOW light " + String(ESP.getChipId(), HEX);
  uint8_t ledType{ENLT_NONE};
  uint8_t coldWhitePin{0};
  uint8_t warmWhitePin{0};
  uint8_t redPin{0};
  uint8_t greenPin{0};
  uint8_t bluePin{0};
} config;

std::vector<espnow_message_t> espnowMessage;

bool ledStatus{false};
uint8_t brightness{255};
uint16_t temperature{255};
uint8_t red{255};
uint8_t green{255};
uint8_t blue{255};

bool wasMqttAvailable{false};

uint8_t gatewayMAC[6]{0};

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

Ticker keepAliveMessageTimer;
bool keepAliveMessageTimerSemaphore{true};
void keepAliveMessageTimerCallback(void);

Ticker statusMessageTimer;
bool statusMessageTimerSemaphore{true};
void statusMessageTimerCallback(void);

void setup()
{
  analogWriteRange(255);

  LittleFS.begin();

  loadConfig();
  loadStatus();

  if (config.coldWhitePin)
    pinMode(config.coldWhitePin, OUTPUT);
  if (config.warmWhitePin)
    pinMode(config.warmWhitePin, OUTPUT);
  if (config.redPin)
    pinMode(config.redPin, OUTPUT);
  if (config.greenPin)
    pinMode(config.greenPin, OUTPUT);
  if (config.bluePin)
    pinMode(config.bluePin, OUTPUT);

  changeLedState();

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  myNet.begin(config.espnowNetName.c_str());
  // myNet.setCryptKey("VERY_LONG_CRYPT_KEY"); // If encryption is used, the key must be set same of all another ESP-NOW devices in network.

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

void onBroadcastReceiving(const char *data, const uint8_t *sender)
{
  esp_now_payload_data_t incomingData;
  memcpy(&incomingData, data, sizeof(esp_now_payload_data_t));
  if (incomingData.deviceType != ENDT_GATEWAY)
    return;
  if (myNet.macToString(gatewayMAC) != myNet.macToString(sender) && incomingData.payloadsType == ENPT_KEEP_ALIVE)
    memcpy(&gatewayMAC, sender, 6);
  if (myNet.macToString(gatewayMAC) == myNet.macToString(sender) && incomingData.payloadsType == ENPT_KEEP_ALIVE)
  {
    isGatewayAvailable = true;
    DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
    deserializeJson(json, incomingData.message);
    bool temp = json["MQTT"] == "online" ? true : false;
    if (wasMqttAvailable != temp)
    {
      wasMqttAvailable = temp;
      if (temp)
      {
        sendConfigMessage();
        sendAttributesMessage();
        sendStatusMessage();
      }
    }
    gatewayAvailabilityCheckTimer.once(15, gatewayAvailabilityCheckTimerCallback);
  }
}

void onUnicastReceiving(const char *data, const uint8_t *sender)
{
  esp_now_payload_data_t incomingData;
  memcpy(&incomingData, data, sizeof(esp_now_payload_data_t));
  if (incomingData.deviceType != ENDT_GATEWAY || myNet.macToString(gatewayMAC) != myNet.macToString(sender))
    return;
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  if (incomingData.payloadsType == ENPT_SET)
  {
    deserializeJson(json, incomingData.message);
    if (json["set"])
      ledStatus = json["set"] == "ON" ? true : false;
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

void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status)
{
  for (uint16_t i{0}; i < espnowMessage.size(); ++i)
  {
    espnow_message_t message = espnowMessage[i];
    if (message.id == id)
    {
      if (status)
        espnowMessage.erase(espnowMessage.begin() + i);
      else
      {
        message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);
        espnowMessage.at(i) = message;
      }
    }
  }
}

void loadConfig()
{
  ETS_GPIO_INTR_DISABLE();
  EEPROM.begin(4096);
  if (EEPROM.read(4095) == 254)
  {
    EEPROM.get(0, config);
    EEPROM.end();
  }
  else
  {
    EEPROM.end();
    saveConfig();
  }
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void saveConfig()
{
  ETS_GPIO_INTR_DISABLE();
  EEPROM.begin(4096);
  EEPROM.write(4095, 254);
  EEPROM.put(0, config);
  EEPROM.end();
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void loadStatus()
{
  ETS_GPIO_INTR_DISABLE();
  if (!LittleFS.exists("/status.json"))
    saveStatus();
  File file = LittleFS.open("/status.json", "r");
  String jsonFile = file.readString();
  DynamicJsonDocument json(192); // To calculate the buffer size uses https://arduinojson.org/v6/assistant.
  deserializeJson(json, jsonFile);
  ledStatus = json["status"];
  brightness = json["brightness"];
  temperature = json["temperature"];
  red = json["red"];
  green = json["green"];
  blue = json["blue"];
  file.close();
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void saveStatus()
{
  ETS_GPIO_INTR_DISABLE();
  DynamicJsonDocument json(128); // To calculate the buffer size uses https://arduinojson.org/v6/assistant.
  json["status"] = ledStatus;
  json["brightness"] = brightness;
  json["temperature"] = temperature;
  json["red"] = red;
  json["green"] = green;
  json["blue"] = blue;
  json["system"] = "empty";
  File file = LittleFS.open("/status.json", "w");
  serializeJsonPretty(json, file);
  file.close();
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void setupWebServer()
{
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/index.htm"); });

  webServer.on("/function.js", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/function.js"); });

  webServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/style.css"); });

  webServer.on("/setting", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        config.ledType = request->getParam("ledType")->value().toInt();
        config.coldWhitePin = request->getParam("coldWhitePin")->value().toInt();
        config.warmWhitePin = request->getParam("warmWhitePin")->value().toInt();
        config.redPin = request->getParam("redPin")->value().toInt();
        config.greenPin = request->getParam("greenPin")->value().toInt();
        config.bluePin = request->getParam("bluePin")->value().toInt();
        config.deviceName = request->getParam("deviceName")->value();
        config.espnowNetName = request->getParam("espnowNetName")->value();
        request->send(200);
        saveConfig(); });

  webServer.on("/config", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        String configJson;
        DynamicJsonDocument json(384); // To calculate the buffer size uses https://arduinojson.org/v6/assistant.
        json["firmware"] = config.firmware;
        json["espnowNetName"] = config.espnowNetName;
        json["deviceName"] = config.deviceName;
        json["ledType"] = config.ledType;
        json["coldWhitePin"] = config.coldWhitePin;
        json["warmWhitePin"] = config.warmWhitePin;
        json["redPin"] = config.redPin;
        json["greenPin"] = config.greenPin;
        json["bluePin"] = config.bluePin;
        serializeJsonPretty(json, configJson);
        request->send(200, "application/json", configJson); });

  webServer.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        request->send(200);
        ESP.restart(); });

  webServer.onNotFound([](AsyncWebServerRequest *request)
                       { request->send(404, "text/plain", "File Not Found"); });

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
  espnow_message_t message;
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  json["Type"] = "ESP-NOW light";
  json["MCU"] = "ESP8266";
  json["MAC"] = myNet.getNodeMac();
  json["Firmware"] = config.firmware;
  json["Library"] = myNet.getFirmwareVersion();
  json["Uptime"] = "Days:" + String(days) + " Hours:" + String(hours - (days * 24)) + " Mins:" + String(mins - (hours * 60));
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void sendKeepAliveMessage()
{
  if (!isGatewayAvailable)
    return;
  keepAliveMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_LED, ENPT_KEEP_ALIVE};
  espnow_message_t message;
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void sendConfigMessage()
{
  if (!isGatewayAvailable)
    return;
  esp_now_payload_data_t outgoingData{ENDT_LED, ENPT_CONFIG};
  espnow_message_t message;
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  json[MCMT_DEVICE_NAME] = config.deviceName;
  json[MCMT_DEVICE_UNIT] = 1;
  json[MCMT_COMPONENT_TYPE] = HACT_LIGHT;
  json[MCMT_DEVICE_CLASS] = config.ledType;
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void sendStatusMessage()
{
  if (!isGatewayAvailable)
    return;
  statusMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_LED, ENPT_STATE};
  espnow_message_t message;
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  json["state"] = ledStatus ? "ON" : "OFF";
  json["brightness"] = brightness;
  json["temperature"] = temperature;
  json["rgb"] = String(red) + "," + String(green) + "," + String(blue);
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

String getValue(String data, char separator, uint8_t index)
{
  uint8_t found{0};
  int strIndex[]{0, -1};
  int maxIndex = data.length() - 1;
  for (uint8_t i{0}; i <= maxIndex && found <= index; i++)
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
      if (config.ledType == ENLT_W || config.ledType == ENLT_RGBW)
        analogWrite(config.coldWhitePin, brightness);
      if (config.ledType == ENLT_WW || config.ledType == ENLT_RGBWW)
      {
        analogWrite(config.coldWhitePin, map(brightness, 0, 255, 0, map(temperature, 500, 153, 0, 255)));
        analogWrite(config.warmWhitePin, map(brightness, 0, 255, 0, map(temperature, 153, 500, 0, 255)));
      }
      if (config.ledType == ENLT_RGB)
      {
        analogWrite(config.redPin, map(red, 0, 255, 0, brightness));
        analogWrite(config.greenPin, map(green, 0, 255, 0, brightness));
        analogWrite(config.bluePin, map(blue, 0, 255, 0, brightness));
      }
      if (config.ledType == ENLT_RGBW || config.ledType == ENLT_RGBWW)
      {
        digitalWrite(config.redPin, LOW);
        digitalWrite(config.greenPin, LOW);
        digitalWrite(config.bluePin, LOW);
      }
    }
    else
    {
      if (config.ledType == ENLT_W)
        analogWrite(config.coldWhitePin, brightness);
      if (config.ledType == ENLT_WW)
      {
        analogWrite(config.coldWhitePin, map(brightness, 0, 255, 0, map(temperature, 500, 153, 0, 255)));
        analogWrite(config.warmWhitePin, map(brightness, 0, 255, 0, map(temperature, 153, 500, 0, 255)));
      }
      if (config.ledType == ENLT_RGBW || config.ledType == ENLT_RGBWW)
        digitalWrite(config.coldWhitePin, LOW);
      if (config.ledType == ENLT_RGBWW)
        digitalWrite(config.warmWhitePin, LOW);
      if (config.ledType == ENLT_RGB || config.ledType == ENLT_RGBW || config.ledType == ENLT_RGBWW)
      {
        analogWrite(config.redPin, map(red, 0, 255, 0, brightness));
        analogWrite(config.greenPin, map(green, 0, 255, 0, brightness));
        analogWrite(config.bluePin, map(blue, 0, 255, 0, brightness));
      }
    }
  }
  else
  {
    if (config.ledType == ENLT_W || config.ledType == ENLT_WW || config.ledType == ENLT_RGBW || config.ledType == ENLT_RGBWW)
      digitalWrite(config.coldWhitePin, LOW);
    if (config.ledType == ENLT_WW || config.ledType == ENLT_RGBWW)
      digitalWrite(config.warmWhitePin, LOW);
    if (config.ledType == ENLT_RGB || config.ledType == ENLT_RGBW || config.ledType == ENLT_RGBWW)
    {
      digitalWrite(config.redPin, LOW);
      digitalWrite(config.greenPin, LOW);
      digitalWrite(config.bluePin, LOW);
    }
  }
  saveStatus();
}

void gatewayAvailabilityCheckTimerCallback()
{
  isGatewayAvailable = false;
  memset(&gatewayMAC, 0, 6);
  espnowMessage.clear();
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