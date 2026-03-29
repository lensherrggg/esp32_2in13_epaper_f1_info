/* Includes ------------------------------------------------------------------*/
#include <stdlib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>

#include "DEV_Config.h"
#include "EPD_2in13_V4.h"
#include "GUI_Paint.h"

// AP Config
const char* AP_SSID = "ESP32_Config";
const char* AP_PASSWORD = "12345678";
const int DNS_PORT = 53;
const int WEB_PORT = 80;
const uint64_t CONNECT_TIMEOUT = 30000;
const uint64_t AP_TIMEOUT = 300000;

WebServer server(WEB_PORT);
DNSServer dnsServer;
Preferences preferences;

bool configMode = false;
uint64_t apStartTime = 0;

// EPaper Config
UBYTE *BlackImage;

const size_t IMAGE_BYTES = 4000;
unsigned char imageBuffer[IMAGE_BYTES];

const int RETRY_TIMES = 20;
const uint64_t CONFIG_MODE_DURATION = 6000000; // 10 minutes
const uint64_t DEEP_SLEEP_SECONDS = 3600;

// f1 info server config
String url = "http://192.168.1.59:5000/f1/display";

void machine_enter_deep_sleep() {
	Serial.println("ESP32 entering sleep mode...");
	esp_sleep_enable_timer_wakeup(DEEP_SLEEP_SECONDS * 1000000);
	esp_deep_sleep_start();
}

bool load_wifi_credentials(String &ssid, String &password) {
	preferences.begin("wifi", false); // 命名空间“wifi”，读写模式
	ssid = preferences.getString("ssid", "");
	password = preferences.getString("password", "");
	preferences.end();
	return (ssid.length() > 0);
}

void save_wifi_credentials(const String &ssid, const String &password) {
	preferences.begin("wifi", false);
	preferences.putString("ssid", ssid);
	preferences.putString("password", password);
	preferences.end();
}

bool connect_wifi(const String &ssid, const String &password) {
	WiFi.mode(WIFI_STA);
	WiFi.disconnect(true);
	WiFi.begin(ssid.c_str(), password);

	Serial.print("Connecting...");
	Serial.printf("SSID: %s\n", ssid);
	Serial.printf("Password: %s\n", password);
	
	int times = 0;
	while (WiFi.status() != WL_CONNECTED && times < RETRY_TIMES) {
		delay(500);
		Serial.print(".");
		times++;
	}
	
	if (times >= RETRY_TIMES) {
		Serial.println("Failed to connect...");
		return false;
	} else {
		Serial.println("Successfully connected to Wi-Fi...");
		Serial.print("IP address: ");
		Serial.println(WiFi.localIP());
		return true;
	}
}

// 处理根路径：显示配置页面
void handle_root() {
    String html = "<!DOCTYPE html><html>";
    html += "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head>";
    html += "<body>";
    html += "<h2>ESP32 WiFi Configuration</h2>";
    html += "<form method=\"POST\" action=\"/configure\">";
    html += "SSID:<br><input type=\"text\" name=\"ssid\" placeholder=\"WiFi SSID\"><br>";
    html += "Password:<br><input type=\"password\" name=\"password\" placeholder=\"Password\"><br><br>";
    html += "<input type=\"submit\" value=\"Save and Restart\">";
    html += "</form></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

// 处理配置表单提交
void handle_configure() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        if (ssid.length() > 0) {
            save_wifi_credentials(ssid, password);
            server.send(200, "text/html", "<h2>Configuration saved. Restarting...</h2>");
            delay(1000);
            ESP.restart();
        } else {
            server.send(400, "text/html", "<h2>Error: SSID cannot be empty</h2>");
        }
    } else {
        server.send(400, "text/html", "<h2>Error: Missing parameters</h2>");
    }
}

// 处理未找到的路径
void handle_not_found() {
    server.send(404, "text/html", "<h2>404 Not Found</h2>");
}

void start_config_mode() {
	Serial.println("Starting config mode...");
	configMode = true;
	apStartTime = millis();
	WiFi.mode(WIFI_AP);
	WiFi.softAP(AP_SSID, AP_PASSWORD);
	WiFi.softAPConfig(
		IPAddress(192, 168, 4, 1),
		IPAddress(192, 168, 4, 1),
		IPAddress(255, 255, 255, 0)
	);
	IPAddress apIP = WiFi.softAPIP();
	Serial.println("AP mode started");
	Serial.print("AP IP address: ");
	Serial.println(apIP);

	// 启动dns server，将所有域名请求指向AP的IP
	dnsServer.start(DNS_PORT, "*", apIP);

	// 配置Web server路由
	server.on("/", handle_root);
	server.on("/configure", HTTP_POST, handle_configure);
	server.onNotFound(handle_not_found);
	server.begin();
	Serial.println("HTTP server started");
}

bool fetch_image(String url) {
	HTTPClient http;
	http.begin(url);
	int httpCode = http.GET();

	if (httpCode != HTTP_CODE_OK) {
		Serial.printf("HTTP GET failed, code: %d\n", httpCode);
		http.end();
		return false;
	}

	int contentLength = http.getSize();
	if (contentLength != IMAGE_BYTES) {
		Serial.printf("Data size mismatch: expected %d, got %d\n", IMAGE_BYTES, contentLength);
		http.end();
		return false;
	}

	WiFiClient* stream = http.getStreamPtr();
	size_t received = 0;
	while (received < IMAGE_BYTES) {
		if (stream->available()) {
			imageBuffer[received] = stream->read();
			received++;
		}
	}

	http.end();

	if (received == IMAGE_BYTES) {
		Serial.println("Image data received successfully. ");
		return true;
	} else {
		Serial.printf("Incomplete data: received %d bytes\n", received);
		return false;
	}
}

void init_display() {
	DEV_Module_Init();

	EPD_2in13_V4_Init();
	// EPD_2in13_V4_Clear();

	
	UWORD Imagesize = ((EPD_2in13_V4_WIDTH % 8 == 0)? (EPD_2in13_V4_WIDTH / 8 ): (EPD_2in13_V4_WIDTH / 8 + 1)) * EPD_2in13_V4_HEIGHT;
	if((BlackImage = (UBYTE *)malloc(Imagesize)) == NULL) 
	{
		printf("Failed to apply for black memory...\r\n");
		while (1);
	}

	Paint_NewImage(BlackImage, EPD_2in13_V4_WIDTH, EPD_2in13_V4_HEIGHT, 90, WHITE);
}

void paint_image() {
	Paint_Clear(WHITE);

	// Paint_DrawBitMap(baseImage);
	Paint_DrawBitMap(imageBuffer);

	EPD_2in13_V4_Display(BlackImage);
}

void paint_string_en(uint16_t Xstart, uint16_t Ystart, const char *pString, sFONT *Font, uint16_t Color_Foreground, uint16_t Color_Background) {
	Paint_Clear(WHITE);
	Paint_DrawString_EN(Xstart, Ystart, pString, Font, Color_Foreground, Color_Background);
	EPD_2in13_V4_Display(BlackImage);
}

void display_goto_sleep() {
	EPD_2in13_V4_Sleep();
	free(BlackImage);
	BlackImage = NULL;
	DEV_Delay_ms(2000);//important, at least 2s
}

/* Entry point ----------------------------------------------------------------*/
void setup()
{
	Serial.begin(115200);

	init_display();

	String ssid, password;

	bool hasConfig = load_wifi_credentials(ssid, password);
	bool wifiConnected = false;
	
	if (hasConfig) {
		// try to connect wifi
		if (wifiConnected = connect_wifi(ssid, password)) {
			if (fetch_image(url)) {
				paint_image();
			} else {
				paint_string_en(10, 0, "Failed to fetch image...", &Font16, WHITE, BLACK);
			}

			DEV_Delay_ms(3000);
			display_goto_sleep();
			
			machine_enter_deep_sleep();
		}
	}

	if (!wifiConnected) {
		paint_string_en(10, 0, "Wi-Fi not configured or connection failure...Please connect to WiFi: ESP32_Config (password: 12345678) Access 192.168.4.1 to configure Wi-Fi", &Font16, WHITE, BLACK);
		DEV_Delay_ms(3000);
		display_goto_sleep();
		start_config_mode();
	}
}

/* The main loop -------------------------------------------------------------*/
void loop()
{
	if(configMode) {
		server.handleClient();
		if (millis() - apStartTime > CONFIG_MODE_DURATION) { // close server after 10 minutes
			Serial.println("Reached configure timeout. Entering deep sleep mode...");
			server.stop();
			machine_enter_deep_sleep();
		}
	}
}
