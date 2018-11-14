#include "ConfigManager.h"

#define MAJORVERSION 0
#define MINORVERSION 2

#define SSID_LENGTH 32
#define SSID_PWD_LENGTH 64
#define HOSTNAME_LENGTH 32

const byte DNS_PORT = 53;
const IPAddress DNS_IP(192, 168, 1, 1);

struct magicHeaderT {
	char magicBytes[2] = { 'C', 'M' };
	byte majorVersion = MAJORVERSION;
	byte minorVersion = MINORVERSION;
};
const magicHeaderT magicHeader;

struct WifiDetails {
	char ssid[SSID_LENGTH];
	char password[SSID_PWD_LENGTH];
	char hostname[HOSTNAME_LENGTH];
};

const char mimeHTML[] PROGMEM = "text/html";
const char mimeJSON[] PROGMEM = "application/json";
const char mimePlain[] PROGMEM = "text/plain";

Mode ConfigManager::getMode() {
    return this->mode;
}

void ConfigManager::setAPName(const char *name) {
    this->apName = (char *)name;
}

void ConfigManager::setAPPassword(const char *password) {
    this->apPassword = (char *)password;
}

void ConfigManager::setAPFilename(const char *filename) {
    this->apFilename = (char *)filename;
}

void ConfigManager::setAPTimeout(const int timeout) {
    this->apTimeout = timeout;
}

void ConfigManager::setWifiConnectRetries(const int retries) {
    this->wifiConnectRetries = retries;
}

void ConfigManager::setWifiConnectInterval(const int interval) {
    this->wifiConnectInterval = interval;
}

void ConfigManager::setAPCallback(std::function<void(WebServer*)> callback) {
    this->apCallback = callback;
}

void ConfigManager::setAPICallback(std::function<void(WebServer*)> callback) {
    this->apiCallback = callback;
}

void ConfigManager::loop() {
    if (mode == ap && apTimeout > 0 && ((millis() - apStart) / 1000) > apTimeout) {
        ESP.restart();
    }

    if (dnsServer) {
        dnsServer->processNextRequest();
    }

    if (server) {
        server->handleClient();
    }
}

void ConfigManager::save() {
    this->writeConfig();
}

JsonObject &ConfigManager::decodeJson(String jsonString)
{
    DynamicJsonBuffer jsonBuffer;

    if (jsonString.length() == 0) {
        return jsonBuffer.createObject();
    }

    JsonObject& obj = jsonBuffer.parseObject(jsonString);

    if (!obj.success()) {
        return jsonBuffer.createObject();
    }

    return obj;
}

void ConfigManager::handleAPGet() {
    SPIFFS.begin();

    File f = SPIFFS.open(apFilename, "r");
    if (!f) {
        Serial.println(F("file open failed"));
        server->send(404, FPSTR(mimeHTML), F("File not found"));
        return;
    }

    server->streamFile(f, FPSTR(mimeHTML));

    f.close();
}

// AccessPoint mode POST method callback will reset ssid, password and hostname in WifiDetails section of EEPROM
void ConfigManager::handleAPPost() {
    bool isJson = server->header("Content-Type") == FPSTR(mimeJSON);
    String ssid;
    String password;
		String hostname;

    if (isJson) {
        JsonObject& obj = this->decodeJson(server->arg("plain"));

        ssid = obj.get<String>("ssid");
        password = obj.get<String>("password");
				hostname = obj.get<String>("hostname");
    } else {
        ssid = server->arg("ssid");
        password = server->arg("password");
				hostname = server->arg("hostname");
    }

    if (ssid.length() == 0 || hostname.length() == 0) {
			server->send(400, FPSTR(mimePlain), F("Invalid ssid or hostname."));
        return;
    }

		WifiDetails wifiDetails;
		strncpy(wifiDetails.ssid, ssid.c_str(), SSID_LENGTH);
		strncpy(wifiDetails.password, password.c_str(), SSID_PWD_LENGTH);
		strncpy(wifiDetails.hostname, hostname.c_str(), HOSTNAME_LENGTH);

		Serial.printf("details.hostname = %s\n\r", wifiDetails.hostname);

		EEPROM.put(0, magicHeader);
		EEPROM.put(sizeof(magicHeader), wifiDetails);

    EEPROM.commit();

		server->send(204, FPSTR(mimePlain), F("Saved. Will attempt to reboot and connect with new ssid and password."));

		delay(1000);

    ESP.restart();
}

void ConfigManager::handleRESTGet() {
    DynamicJsonBuffer jsonBuffer;
    JsonObject& obj = jsonBuffer.createObject();

    std::list<BaseParameter*>::iterator it;
    for (it = parameters.begin(); it != parameters.end(); ++it) {
        if ((*it)->getMode() == set) {
            continue;
        }

        (*it)->toJson(&obj);
    }

    String body;
    obj.printTo(body);

    server->send(200, FPSTR(mimeJSON), body);
}

void ConfigManager::handleRESTPut() {
    JsonObject& obj = this->decodeJson(server->arg("plain"));
    if (!obj.success()) {
        server->send(400, FPSTR(mimeJSON), "");
        return;
    }

    std::list<BaseParameter*>::iterator it;
    for (it = parameters.begin(); it != parameters.end(); ++it) {
        if ((*it)->getMode() == get) {
            continue;
        }

        (*it)->fromJson(&obj);
    }

    writeConfig();

    server->send(204, FPSTR(mimeJSON), "");
}

void ConfigManager::handleNotFound() {
	if (IPAddress().isValid(server->hostHeader())) {
		server->sendHeader("Location", String("http://") + server->client().localIP().toString(), true);
        server->send(302, FPSTR(mimePlain), ""); // Empty content inhibits Content-length header so we have to close the socket ourselves.
        server->client().stop();
        return;
    }

    server->send(404, FPSTR(mimePlain), "");
    server->client().stop();
}

// cycle <wifiConnectRetries> with a delay of <wifiConnectInterval> milliseconds
// return true when connected
bool ConfigManager::wifiConnected() {
		Serial.printf("sizeof(MagicHeader) %d, sizeof(WifiDetails), %d\r\n", sizeof(magicHeader), sizeof(WifiDetails));
    Serial.print(F("Waiting for WiFi to connect"));

    int i = 0;
    while (i < wifiConnectRetries) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("");
            return true;
        }

        Serial.print(".");

        delay(wifiConnectInterval);
        i++;
    }

    Serial.println("");
    Serial.println(F("Connection timed out"));

    return false;
}

void ConfigManager::setup(const uint32_t *hostnamePostfix) {

	Serial.println(F("Reading saved configuration"));

	// read the MagicHeader (4 bytes: 'C','M',<major>,<minor>)
	magicHeaderT header;
	EEPROM.get(0, header);

	Serial.printf("Header: v%d.%d %2sn\r", header.majorVersion, header.minorVersion, header.magicBytes);

	// compare the MagicHeader from EEPROM with the one defined in code
	// if they're not the same then start in AccessPoint mode to get ssid, password and hostname
	if (memcmp(&header, &magicHeader, sizeof(magicHeader)) == 0) {

		// read ssid, password and hostname from EEPROM
		WifiDetails details;
		EEPROM.get(sizeof(magicHeader), details);

		Serial.printf("wifiDetails: <%32s> <%32s> <%64s>\n\r", details.hostname, details.ssid, details.password);

		readConfig();

		// add "_<ChipID>" to hostname
		if (hostnamePostfix != NULL) {
			char newHostname[64];
			snprintf(newHostname, 64, "%s_%0X", details.hostname, *hostnamePostfix);
			WiFi.hostname(newHostname);
		} else {
			WiFi.hostname(details.hostname);
		}

		// attempt to connect to previously configured network (ssid, password)
		WiFi.begin(details.ssid, details.password[0] == '\0' ? NULL : details.password);

		// attempt to connect
		if (wifiConnected()) {
			Serial.printf("Connected to %s as %s (%s)\r\n", details.ssid, WiFi.hostname().c_str(), WiFi.localIP().toString().c_str());

			WiFi.mode(WIFI_STA);
			startApi();
			return;
		} else {
			// unable to connect with stored wifi configuration, start AccessPoint
			Serial.printf("Failed to connect to %s\r\nPlease connect to TemperatureReader wifi AccessPoint and browse to http://192.168.1.1\r\n", details.ssid, DNS_IP.toString().c_str());
		}
	} else {
		Serial.printf("current version [%x.%x] does not match application version [%x.%x]\r\n", header.majorVersion, header.minorVersion, magicHeader.majorVersion,
				magicHeader.minorVersion);
	}

	// We are at a cold start, don't bother timeing out.
	apTimeout = 0;
	startAP();
	return;

}

void ConfigManager::startAP() {
	const char* headerKeys[] = {"Content-Type"};
	size_t headerKeysSize = sizeof(headerKeys)/sizeof(char*);

	mode = ap;

	Serial.printf("Starting Access Point %s\r\n", DNS_IP.toString().c_str());

	WiFi.mode(WIFI_AP);
	WiFi.softAP(apName, apPassword);

	delay(500); // Need to wait to get IP

	IPAddress NMask(255, 255, 255, 0);
	WiFi.softAPConfig(DNS_IP, DNS_IP, NMask);

	Serial.printf("AP IP address: %s\r\n", WiFi.softAPIP().toString().c_str());

	dnsServer.reset(new DNSServer);
	dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
	dnsServer->start(DNS_PORT, "*", DNS_IP);

	server.reset(new WebServer(80));
	server->collectHeaders(headerKeys, headerKeysSize);
	server->on("/", HTTPMethod::HTTP_GET, std::bind(&ConfigManager::handleAPGet, this));
	server->on("/", HTTPMethod::HTTP_POST, std::bind(&ConfigManager::handleAPPost, this));
	server->onNotFound(std::bind(&ConfigManager::handleNotFound, this));

	if (apCallback) {
		apCallback(server.get());
	}

	server->begin();

	apStart = millis();
}

void ConfigManager::startApi() {
	const char* headerKeys[] = {"Content-Type"};
	size_t headerKeysSize = sizeof(headerKeys)/sizeof(char*);

    mode = api;

    server.reset(new WebServer(80));
    server->collectHeaders(headerKeys, headerKeysSize);
    server->on("/", HTTPMethod::HTTP_GET, std::bind(&ConfigManager::handleAPGet, this));
    server->on("/", HTTPMethod::HTTP_POST, std::bind(&ConfigManager::handleAPPost, this));
    server->on("/settings", HTTPMethod::HTTP_GET, std::bind(&ConfigManager::handleRESTGet, this));
    server->on("/settings", HTTPMethod::HTTP_PUT, std::bind(&ConfigManager::handleRESTPut, this));
    server->onNotFound(std::bind(&ConfigManager::handleNotFound, this));

    if (apiCallback) {
        apiCallback(server.get());
    }

    server->begin();
}

void ConfigManager::readConfig() {
	byte *ptr = (byte *) config;

	Serial.printf("configSize = %d\n\r",configSize);

	for (int i = 0, line = 0; i < configSize; i++) {

		if (i % 16 == 0) Serial.printf("\n\r%08x: ", ptr+i);
		if (i % 8 == 0) Serial.printf("     ");

		ptr[i] = EEPROM.read(CONFIG_OFFSET + i);

		Serial.printf("%02X ", ptr[i]);
	}

	Serial.printf("\n\r\n\r");

	if (printCallback != NULL) printCallback(ptr);


}

void ConfigManager::writeConfig() {
	byte *ptr = (byte *) config;

	for (int i = 0; i < configSize; i++) {
		EEPROM.write(CONFIG_OFFSET + i, ptr[i]);
	}
	EEPROM.commit();
	if (printCallback != NULL) printCallback(ptr);
}
