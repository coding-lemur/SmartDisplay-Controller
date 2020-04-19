#include <FS.h> // include the SPIFFS library
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClient.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>
#include <Fonts/TomThumb.h>
#include <LightDependentResistor.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <Wire.h>
//#include "SoftwareSerial.h"
#include <WiFiManager.h>
#include <DoubleResetDetect.h>
#include <EasyButton.h>
#include "SmartDisplay-conf.h"

String version = "0.13.0 beta";

// settings
bool USBConnection = false;
bool MatrixType2 = false;
char mqtt_server[16];
uint16_t mqtt_port;
char mqtt_user[16];
char mqtt_password[16];

bool configLoaded = false;
bool connectedWithServer = false;
bool shouldSaveConfig = false;
bool updating = false;
bool powerOn = true;
bool poweringOff = false;
bool isMqttConnecting = false;

unsigned long myTime; // need for animation
int myCounter;

// timers
unsigned long lastBrightnessCheck = 0;
unsigned long lastMessageFromServer = 0;
unsigned long lastInfoSend = 0;

AsyncMqttClient mqttClient;
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;

WiFiManager wifiManager;

ESP8266WebServer server(80);
const char *serverIndex = "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";

LightDependentResistor photocell(LDR_PIN, LDR_RESISTOR, LDR_PHOTOCELL);

DHT dht(DHT_PIN, DHT_TYPE);
EasyButton button(BUTTON_PIN);
DoubleResetDetect drd(DRD_TIMEOUT, DRD_ADDRESS);

CRGB leds[256];
FastLED_NeoMatrix *matrix;

#ifndef ICACHE_RAM_ATTR
#define ICACHE_RAM_ATTR IRAM_ATTR
#endif

void sendInfo()
{
	DynamicJsonDocument root(1024);
	root["version"] = version;
	root["chipID"] = String(ESP.getChipId());
	root["lux"] = static_cast<int>(round(photocell.getCurrentLux()));
	root["powerOn"] = powerOn;

	// network
	JsonObject network = root.createNestedObject("network");
	network["wifirssi"] = WiFi.RSSI();
	network["wifiquality"] = GetRSSIasQuality(WiFi.RSSI());
	network["wifissid"] = WiFi.SSID();
	network["ip"] = WiFi.localIP().toString();

	// room weather
	JsonObject roomWeather = root.createNestedObject("roomWeather");
	roomWeather["humidity"] = dht.readHumidity();
	roomWeather["temperature"] = dht.readTemperature();

	String JS;
	serializeJson(root, JS);

	if (USBConnection)
	{
		Serial.println(String(JS));
	}
	else
	{
		mqttClient.publish("smartDisplay/client/out/info", 0, true, JS.c_str());
	}

	lastInfoSend = millis();
}

void reconnect()
{
	if (isMqttConnecting)
	{
		return;
	}

	log("reconnecting to " + String(mqtt_server) + ":" + String(mqtt_port));

	String clientId = "SmartDisplay-";
	clientId += String(random(0xffff), HEX);

	//hardwareAnimatedSearch(1, 28, 0);

	// connect to MQTT broker
	isMqttConnecting = true;
	mqttClient.connect();
}

void hardReset()
{
	matrix->clear();
	matrix->setTextColor(matrix->Color(255, 0, 0));
	matrix->setCursor(6, 6);
	matrix->print("RESET!");
	matrix->show();

	delay(1000);

	// remove config file
	SPIFFS.remove(CONFIG_FILE);
	log("settings removed");

	delay(1000);

	// reset settings
	wifiManager.resetSettings();
	ESP.reset();
}

void onButtonPressed()
{
	if (!mqttClient.connected())
	{
		return;
	}

	mqttClient.publish("smartDisplay/client/out/button", 0, true, "pressed");
}

void onButtonPressedForDuration()
{
	hardReset();
}

void saveConfigCallback()
{
	log("Should save config");
	shouldSaveConfig = true;
}

void configModeCallback(WiFiManager *myWiFiManager)
{
	log("Entered config mode");
	log(myWiFiManager->getConfigPortalSSID());

	matrix->clear();
	matrix->setCursor(3, 6);
	matrix->setTextColor(matrix->Color(0, 255, 50));
	matrix->print("Hotspot");
	matrix->show();
}

void checkBrightness()
{
	if (millis() - lastBrightnessCheck >= 10000) // check every 10 sec
	{
		return;
	}

	float lux = photocell.getCurrentLux();
	long brightness = 255;

	if (lux < 50)
	{
		brightness = map(lux, 0, 50, 20, 255);
	}

	matrix->setBrightness(brightness);
	lastBrightnessCheck = millis();
}

void checkServerIsOnline()
{
	if (!powerOn)
	{
		return;
	}

	if (millis() - lastMessageFromServer >= 60000) // more than one minute no message from server
	{
		matrix->clear();
		matrix->drawLine(0, 3, 31, 3, matrix->Color(255, 0, 0));
		matrix->show();
	}
}

void setup()
{
	delay(2000);

	Serial.setRxBufferSize(1024);
	Serial.begin(115200);

	log("setup");
	log(version);

	// try to read settings
	if (SPIFFS.begin())
	{
		// if file not exists
		if (!SPIFFS.exists(CONFIG_FILE))
		{
			SPIFFS.open(CONFIG_FILE, "w+");
			log("make File...");
		}

		File configFile = SPIFFS.open(CONFIG_FILE, "r");

		if (configFile)
		{
			DynamicJsonDocument doc(1024);
			DeserializationError error = deserializeJson(doc, configFile);

			bool isSuccessfulParsed = error.code() == DeserializationError::Ok;

			if (isSuccessfulParsed)
			{
				log("parsed json");

				loadConfig(doc);
			}

			configFile.close();
		}
	}
	else // not possible to read settings file
	{
		log("mounting not possible");
	}

	log("Matrix Type");

	if (MatrixType2)
	{
		matrix = new FastLED_NeoMatrix(leds, 32, 8, NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG);
	}
	else
	{
		matrix = new FastLED_NeoMatrix(leds, 32, 8, NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG);
	}

	// TODO maybe we can implement setTemperature ("matrixTempCorrection")
	FastLED
		.addLeds<NEOPIXEL, MATRIX_DATA_PIN>(leds, 256)
		.setCorrection(TypicalLEDStrip);

	matrix->begin();
	matrix->setTextWrap(false);
	matrix->setBrightness(80);
	matrix->setFont(&TomThumb);

	if (drd.detect())
	{
		// double reset -> hard reset

		log("** Double reset boot **");

		hardReset();
	}

	wifiManager.setAPStaticIPConfig(IPAddress(172, 217, 28, 1), IPAddress(172, 217, 28, 1), IPAddress(255, 255, 255, 0));

	WiFiManagerParameter custom_mqtt_server("mqtt_server", "MQTT server", mqtt_server, 16);
	WiFiManagerParameter custom_mqtt_port("mqtt_port", "MQTT port", "1883", 6);
	WiFiManagerParameter custom_mqtt_user("mqtt_user", "MQTT user", mqtt_user, 16);
	WiFiManagerParameter custom_mqtt_password("mqtt_password", "MQTT password", mqtt_password, 16);
	WiFiManagerParameter p_MatrixType2("MatrixType2", "MatrixType 2", "T", 2, "type=\"checkbox\" ", WFM_LABEL_BEFORE);
	WiFiManagerParameter p_USBConnection("USBConnection", "Serial Connection", "T", 2, "type=\"checkbox\" ", WFM_LABEL_BEFORE);

	// Just a quick hint
	WiFiManagerParameter p_hint("<small>Please configure your SmartDisplay Server IP (without Port), and check MatrixType 2 if you cant read anything on the Matrix<br></small><br><br>");
	WiFiManagerParameter p_lineBreak_notext("<p></p>");

	wifiManager.setSaveConfigCallback(saveConfigCallback);
	wifiManager.setAPCallback(configModeCallback);
	wifiManager.setHostname(HOST_NAME);
	wifiManager.addParameter(&p_hint);
	wifiManager.addParameter(&custom_mqtt_server);
	wifiManager.addParameter(&p_lineBreak_notext);
	wifiManager.addParameter(&custom_mqtt_port);
	wifiManager.addParameter(&custom_mqtt_user);
	wifiManager.addParameter(&custom_mqtt_password);
	wifiManager.addParameter(&p_lineBreak_notext);
	wifiManager.addParameter(&p_MatrixType2);
	wifiManager.addParameter(&p_USBConnection);
	wifiManager.addParameter(&p_lineBreak_notext);

	wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
	wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

	if (!USBConnection)
	{
		mqttClient.onConnect(onMqttConnect);
		mqttClient.onDisconnect(onMqttDisconnect);
		mqttClient.onMessage(onMqttMessage);

		mqttClient.setServer(mqtt_server, mqtt_port);
		// TODO maybe check user and password was set
		mqttClient.setCredentials(mqtt_user, mqtt_password);
	}

	hardwareAnimatedSearch(0, 24, 0);

	if (!wifiManager.autoConnect(HOTSPOT_SSID, HOTSPOT_PASSWORD))
	{
		log("failed to connect and hit timeout");
		delay(3000);

		// reset and try again, or maybe put it to deep sleep
		ESP.reset();
		delay(5000);
	}

	//log("connected to server: " + String(mqtt_server) + ":" + String(mqtt_port));

	server.on("/", HTTP_GET, []() {
		server.sendHeader("Connection", "close");
		server.send(200, "text/html", serverIndex);
	});
	server.on(
		"/update", HTTP_POST, []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
      ESP.restart(); }, []() {
      HTTPUpload& upload = server.upload();
	  
      if (upload.status == UPLOAD_FILE_START) {
        Serial.setDebugOutput(true);
		if(!USBConnection){
			Serial.printf("Update: %s\n", upload.filename.c_str());	
		}
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSketchSpace)) { //start with max available size
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
		  matrix->clear();
		  flashProgress((int)upload.currentSize,(int)upload.buf);
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) { //true to set the size to the current progress
		  server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
		  if(!USBConnection){
			Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
			}
          
        } else {
          Update.printError(Serial);
        }
        Serial.setDebugOutput(false);
      }
      yield(); });

	server.begin();

	if (shouldSaveConfig)
	{
		log("saving config");

		strcpy(mqtt_server, custom_mqtt_server.getValue());
		mqtt_port = strtoul(custom_mqtt_port.getValue(), NULL, 10);
		strcpy(mqtt_user, custom_mqtt_user.getValue());
		strcpy(mqtt_password, custom_mqtt_password.getValue());
		MatrixType2 = (strncmp(p_MatrixType2.getValue(), "T", 1) == 0);
		USBConnection = (strncmp(p_USBConnection.getValue(), "T", 1) == 0);

		saveConfig();
		ESP.reset();
	}

	hardwareAnimatedCheck(0, 27, 2);

	delay(1000);

	ArduinoOTA.onStart([&]() {
		updating = true;
		matrix->clear();
	});

	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
		flashProgress(progress, total);
	});

	ArduinoOTA.begin();

	// init sensors

	photocell.setPhotocellPositionOnGround(false);

	dht.begin();

	button.begin();
	button.onPressed(onButtonPressed);
	button.onPressedFor(12000, onButtonPressedForDuration);

	matrix->clear();
	matrix->setCursor(6, 6);
	matrix->setTextColor(matrix->Color(0, 255, 0));
	matrix->print("Ready!");
	matrix->show();

	myTime = millis() - 500;
	myCounter = 0;
}

void loop()
{
	server.handleClient(); // TODO disable server after init setup finished
	ArduinoOTA.handle();

	checkBrightness();

	if (!connectedWithServer)
	{
		if (USBConnection)
		{
			if (millis() - myTime > 100)
			{
				serverSearch(myCounter, 1, 28, 0);
				myCounter++;

				if (myCounter == 13)
				{
					myCounter = 0;
				}

				myTime = millis();
			}
		}
		else
		{
			if (millis() - myTime > 500)
			{
				serverSearch(myCounter, 0, 28, 0);
				myCounter++;

				if (myCounter == 4)
				{
					myCounter = 0;
				}

				myTime = millis();
			}
		}
	}

	if (!updating)
	{
		if (USBConnection)
		{
			while (Serial.available() > 0)
			{
				connectedWithServer = true;
				String message = Serial.readStringUntil('}') + "}";

				DynamicJsonDocument doc(1024);
				deserializeJson(doc, message);

				String type = doc["type"];
				processing(type, doc);
			};
		}
		else // wifi
		{
			if (mqttClient.connected())
			{
				connectedWithServer = true;

				if (lastInfoSend == 0 || millis() - lastInfoSend >= 45000) // every 45 seconds
				{
					sendInfo();
				}
			}
			else
			{
				reconnect();
			}
		}

		checkServerIsOnline();

		if (poweringOff)
		{
			// clear screen
			matrix->clear();
			matrix->show();

			poweringOff = false;
		}

		button.read();
	}
}
