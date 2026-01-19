#include <Arduino.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <NeoPixelBus.h>
#include <NeoPixelAnimator.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>

const uint32_t maxMilliAmps = 1000;

// define local url
// http://<this string>.local
const String localUrl = "leds";

// preconfigured network login credentials
const String PCssid = "223-WFUN-2-2.4";
const String PCpassword = "Vexhsk100";

// set this to true to have the board host its own network.
// if it cannot find/connect to the specified network, it will give up and host a network
bool hotspot = false;

// hotspot network login credentials
const String HSssid = "lighting-overthonked";
const String HSpassword = "12341234";

IPAddress local_IP(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// set webserver to use port 80 (http) and /ws for websocket connections
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static AsyncRateLimitMiddleware rateLimit;

const uint16_t PixelCount = 300;

// There are two ways of driving the neopixel strip. DMA has a lot less CPU overhead, but the DMA
// pin is used when flashing over USB causing the lights to freak out and sometimes draw a
// problematic amount of current. To fix this, I use the slower UART method when not hooked up to a
// proper power supply.
NeoPixelBus<NeoGrbFeature, NeoEsp8266AsyncUart1Ws2812xMethod> strip(PixelCount);
// NeoPixelBus<NeoGrbFeature, NeoEsp8266DmaWs2812xMethod> strip(PixelCount);

NeoGamma<NeoGammaTableMethod> colorGamma;

File initJson;

unsigned long lastCommandMillis = 0;
bool commandDirty = false;
String lastCommandString = "";

bool animationRunning = false;

void jsonCommand(const String& json) {
	JsonDocument doc;
	DeserializationError err = deserializeJson(doc, json);

	// Print received JSON for debugging
	Serial.print("Received JSON: ");
	Serial.println(json);

	if (err) {
		Serial.println("JSON parse error");
		return;
	}

	// Save command string
	lastCommandString = json;

	// icky elif chain but it really doesn't matter
	if (strcmp(doc["cmd"] | "", "setAllHSV") == 0) {
		float h = doc["h"];
		float s = doc["s"];
		float v = doc["v"];

		if (strip.CanShow()) {
			strip.ClearTo(colorGamma.Correct(RgbColor(HsbColor(h, s, v))));
			strip.Show();
		}

		animationRunning = false;
	}
	else if (strcmp(doc["cmd"] | "", "setAllRGB") == 0) {
		int r = doc["r"];
		int g = doc["g"];
		int b = doc["b"];

		Serial.print("SetAllRGB: ");
		Serial.print(r);
		Serial.print(", ");
		Serial.print(g);
		Serial.print(", ");
		Serial.print(b);

		strip.ClearTo(RgbColor(r, g, b));
		strip.Show();

		animationRunning = false;
	} else if (strcmp(doc["cmd"] | "", "startAnimation") == 0) {
		animationRunning = true;
	}
}

void onWsEvent(
	AsyncWebSocket* server,
	AsyncWebSocketClient* client,
	AwsEventType type,
	void* arg,
	uint8_t* data,
	size_t len) {
	if (type == WS_EVT_DATA) {
		AwsFrameInfo* info = (AwsFrameInfo*)arg;

		if (info->opcode == WS_TEXT && info->final) {
			{
				char* buf = (char*)malloc(len + 1);
				if (buf) {
					memcpy(buf, data, len);
					buf[len] = 0;
					String payload = String(buf);
					free(buf);
					jsonCommand(payload);
				} else {
					jsonCommand(String());
				}
			}
			// update last command timestamp
			lastCommandMillis = millis();
			commandDirty = true;
		}
	}
}

void setup() {
	pinMode(LED_BUILTIN, OUTPUT);

	Serial.begin(115200);
	while (!Serial); // wait for serial attach
	Serial.println("\nSerial attached.\n");

	Serial.println("Clearing LEDs...");
	strip.Begin();
	strip.Show();

	Serial.print("\nMounting filesystem... ");
	if (!LittleFS.begin()) {
		Serial.println("Failed to mount filesystem.");
	}
	else {
		Serial.println("Mounted!");
	}

	Serial.println("Loading most recent command...");
	// check for init.json file
	if (!LittleFS.exists("/init.json")) {
		Serial.println("init.json not found, making new one.");
		initJson = LittleFS.open("/init.json", "w");
		initJson.println("{\"cmd\":\"setAllHSV\",\"h\":0.129,\"s\":0.549,\"v\":1}");
		initJson.close();
		strip.ClearTo(colorGamma.Correct(RgbColor(HsbColor(0.129, 0.549, 1))));
		strip.Show();
	}
	else {
		Serial.println("init.json found, running command.");
		initJson = LittleFS.open("/init.json", "r");
		String saved = initJson.readString();
		initJson.close();
		Serial.println(saved);
		jsonCommand(saved);
	}

	Serial.println(lastCommandString);

	// TODO separate wifi and webserver init into separate function/file
	if (!hotspot) {
		Serial.println("\nScanning for networks (2.4 GHz ONLY)...");
		WiFi.mode(WIFI_STA);
		WiFi.disconnect();
		delay(100);

		int n = WiFi.scanNetworks();
		Serial.print(n);
		Serial.println(" networks found:");

		if (n == 0) {
			hotspot = true;
		}
		else {
			bool ssidMatch = false;

			for (int i = 0; i < n; ++i) {
				// Check if the current network matches the configured SSID
				String currSSID = WiFi.SSID(i);
				if (!ssidMatch) {
					ssidMatch = currSSID.equals(PCssid);
				}

				// Print SSID and RSSI (signal strength)
				Serial.print(i + 1);
				Serial.print(": ");
				Serial.print(currSSID);
				Serial.print(" (");
				Serial.print(WiFi.RSSI(i));
				Serial.println(" dBm)");
				delay(10);
			}

			Serial.println();

			// switch to hosting a network if preconfigured one is not found
			if (!ssidMatch) {
				hotspot = true;
				Serial.print(PCssid);
				Serial.println(" was not found; it may not have 2.4 GHz.");
			}
		}
	}

	if (!hotspot) {
		WiFi.begin(PCssid, PCpassword);
		Serial.print("Connecting to WiFi network: ");
		Serial.print(PCssid);

		int waitSecs = 5;
		for (int i = 0; WiFi.waitForConnectResult() != WL_CONNECTED && i < waitSecs; i++) {
			Serial.print(".");
			digitalWrite(LED_BUILTIN, LOW);
			delay(500);
			digitalWrite(LED_BUILTIN, HIGH);
			delay(500);
		}
		Serial.println();

		if (WiFi.waitForConnectResult() != WL_CONNECTED) {
			Serial.println("Connection timeout.");
			hotspot = true;
		}
		else {
			Serial.println("Connected to WiFi!");
			Serial.print("IP Address: http://");
			Serial.println(WiFi.localIP());
		}
	}

	if (hotspot) {
		digitalWrite(LED_BUILTIN, LOW);
		Serial.println("\nSwitching to AP mode...");

		WiFi.mode(WIFI_AP);
		WiFi.softAPConfig(local_IP, gateway, subnet);
		bool APsuccess = WiFi.softAP(HSssid, HSpassword);

		if (APsuccess) {
			digitalWrite(LED_BUILTIN, HIGH);
			Serial.println("Access point created!");
			Serial.print("SSID: ");
			Serial.println(HSssid);
			Serial.print("Passkey: ");
			Serial.println(HSpassword);
			Serial.print("IP Address: http://");
			Serial.println(WiFi.softAPIP());
		}
		else {
			Serial.println("Access point could not be created. Check the configured credentials.");
		}
	}

	Serial.print("\nInitializing webserver... ");
	rateLimit.setMaxRequests(5);
	rateLimit.setWindowSize(10);

	// serves all static files from the web folder.
	server.serveStatic("/", LittleFS, "/web").setDefaultFile("index.html");
	server.begin();
	ws.onEvent(onWsEvent);
	server.addHandler(&ws);
	Serial.println(" Server is up!");

	Serial.print("\nBroadcasting mDNS... ");
	if (MDNS.begin(localUrl)) {
		Serial.print("Success!\nInterface can be accessed at http://");
		Serial.print(localUrl);
		Serial.println(".local");
	}
	else {
		Serial.print("Failure!\nYou will have to access the interface using the IP address.\nhttp://");
		if (hotspot) {
			Serial.println(WiFi.softAPIP());
		}
		else {
			Serial.println(WiFi.localIP());
		}
	}

	Serial.print("\nSetting up OTA updates... ");
	ArduinoOTA.onStart([]() {
		String type;
		if (ArduinoOTA.getCommand() == U_FLASH) {
			type = "sketch";
		}
		else {	// U_FS
			type = "filesystem";
		}

		LittleFS.end();
		Serial.println("Start updating " + type);
		});

	ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
		Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
	});
	ArduinoOTA.onError([](ota_error_t error) {
		Serial.printf("Error[%u]: ", error);
		if (error == OTA_AUTH_ERROR) {
			Serial.println("Auth Failed");
		}
		else if (error == OTA_BEGIN_ERROR) {
			Serial.println("Begin Failed");
		}
		else if (error == OTA_CONNECT_ERROR) {
			Serial.println("Connect Failed");
		}
		else if (error == OTA_RECEIVE_ERROR) {
			Serial.println("Receive Failed");
		}
		else if (error == OTA_END_ERROR) {
			Serial.println("End Failed");
		} });
		ArduinoOTA.begin();
		Serial.println("OTA ready.");
}

RgbColor grad1(HtmlColor(0x3B20FF));
RgbColor grad2(HtmlColor(0xFF1CFF));
RgbColor grad3(HtmlColor(0x665AFF));

float phase1 = 0.0f;
float phase2 = 0.0f;
float phase3 = 0.0f;

void loop() {
	ws.cleanupClients();
	ArduinoOTA.handle();
	MDNS.update();

	// check time since last command
	// if more than 5 seconds, save last command to init.json
	if (millis() - lastCommandMillis > 5000) {
		if (commandDirty) {
			Serial.println("Saving last command to init.json");
			Serial.println(lastCommandString);
			initJson = LittleFS.open("/init.json", "w");
			initJson.print(lastCommandString);
			initJson.close();
			commandDirty = false;
		}
	}

	//Serial.println(animationRunning);

	if (animationRunning) {
		// calculate sine values and normalize them
		float sin1 = (sinf(phase1) + 1)/2;
		float sin2 = (sinf(phase2) + 1)/2;
		float sin3 = (sinf(phase3) + 1)/2;

		Serial.print("Sines: ");
		Serial.print(sin1);
		Serial.print(", ");
		Serial.print(sin2);
		Serial.print(", ");
		Serial.print(sin3);
		Serial.print(" | RGB: ");

		// mix gradient colors based on sine phases
		float r = grad1.R * sin1 + grad2.R * sin2 + grad3.R * sin3;
		float g = grad1.G * sin1 + grad2.G * sin2 + grad3.G * sin3;
		float b = grad1.B * sin1 + grad2.B * sin2 + grad3.B * sin3;

		// normalize based on Blue. Blue should always be 255.
		float normFactor = 255.0f / b;
		r *= normFactor;
		g *= normFactor;
		b *= normFactor;

		// boost red slightly
		float rDefeciet = 255.0f - r;
		r += rDefeciet * 0.5f;

		// but don't let it get too crazy
		r -= 20.0f;

		Serial.print(r);
		Serial.print(", ");
		Serial.print(g);
		Serial.print(", ");
		Serial.println(b);

		// display mixed color on first pixel and shift
		RgbColor mixedColor = RgbColor((uint8_t)r, (uint8_t)g, (uint8_t)b);

		strip.SetPixelColor(0, colorGamma.Correct(mixedColor));
		strip.ShiftRight(1);
		strip.Show();

		// increment phases
		phase1 += 0.02f;
		phase2 += 0.025f;
		phase3 += 0.03f;

		// wrap phases
		if (phase1 > TWO_PI) phase1 -= TWO_PI;
		if (phase2 > TWO_PI) phase2 -= TWO_PI;
		if (phase3 > TWO_PI) phase3 -= TWO_PI;

		delay(20);
	}
}