#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "../../Configuration/Constants.h"
#include "../../Configuration/Settings.h"
#include "../../Configuration/Version.h"
#include "NetworkScanner.h"
#include "../../Utils/StaticFile.h"
#include "../Logger/Logger.h"
#include "WebServer.h"
#include "../Wallbox/IWallbox.h"

// Static files
#include "../../../data/headers/blueberry.svg.h"
#include "../../../data/headers/favicon-16x16.png.h"
#include "../../../data/headers/favicon-32x32.png.h"
#include "../../../data/headers/heidelbridge.css.h"
#include "../../../data/headers/index.html.h"
#include "../../../data/headers/index.js.h"
#include "../../../data/headers/modal.js.h"
#include "../../../data/headers/pico.violet.min.css.h"
#include "../../../data/headers/update.html.h"
#include "../../../data/headers/update.js.h"

StaticFile staticFiles[] = {
    {"/", "text/html", index_html, index_html_len},
    {"/index.js", "application/javascript", index_js, index_js_len},
    {"/update", "text/html", update_html, update_html_len},
    {"/update.js", "application/javascript", update_js, update_js_len},
    {"/modal.js", "application/javascript", modal_js, modal_js_len},
    {"/heidelbridge.css", "text/css", heidelbridge_css, heidelbridge_css_len},
    {"/pico.violet.min.css", "text/css", pico_violet_min_css, pico_violet_min_css_len},
    {"/blueberry.svg", "image/svg+xml", blueberry_svg, blueberry_svg_len},
    {"/favicon-16x16.png", "image/png", favicon_16x16_png, favicon_16x16_png_len},
    {"/favicon-32x32.png", "image/png", favicon_32x32_png, favicon_32x32_png_len},
};

extern IWallbox* gWallbox;

AsyncWebServer gWebServer(Constants::WebServer::Port);

WebServer *WebServer::Instance()
{
    static WebServer instance;
    return &instance;
}

// Initializes the asynchronous web server
void WebServer::Init()
{
    if (!Constants::WebServer::Enabled)
    {
        Logger::Info("Web server is disabled");
        return;
    }

    Logger::Info("Initializing web server");

    // Required
    gWebServer.on("/connecttest.txt", [](AsyncWebServerRequest *request)
                  { request->redirect("http://logout.net"); }); // windows 11 captive portal workaround
    gWebServer.on("/wpad.dat", [](AsyncWebServerRequest *request)
                  { request->send(404); }); // Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32 :)

    // Background responses: Probably not all are Required, but some are. Others might speed things up?
    // A Tier (commonly used by modern systems)
    gWebServer.on("/generate_204", [](AsyncWebServerRequest *request)
                  { request->redirect(Constants::WebServer::LocalIpUrl); }); // android captive portal redirect
    gWebServer.on("/redirect", [](AsyncWebServerRequest *request)
                  { request->redirect(Constants::WebServer::LocalIpUrl); }); // microsoft redirect
    gWebServer.on("/hotspot-detect.html", [](AsyncWebServerRequest *request)
                  { request->redirect(Constants::WebServer::LocalIpUrl); }); // apple call home
    gWebServer.on("/canonical.html", [](AsyncWebServerRequest *request)
                  { request->redirect(Constants::WebServer::LocalIpUrl); }); // firefox captive portal call home
    gWebServer.on("/success.txt", [](AsyncWebServerRequest *request)
                  { request->send(200); }); // firefox captive portal call home
    gWebServer.on("/ncsi.txt", [](AsyncWebServerRequest *request)
                  { request->redirect(Constants::WebServer::LocalIpUrl); }); // windows call home

    // Handle API requests
    gWebServer.on("/api/version", HTTP_GET, [this](AsyncWebServerRequest *request)
                  { request->send(200, "application/json", HandleApiRequestGetVersion()); });
    gWebServer.on("/api/wifi_scan_start", HTTP_POST, [this](AsyncWebServerRequest *request)
                  { request->send(200, "application/json", HandleApiRequestStartWifiScan()); });
    gWebServer.on("/api/wifi_scan_status", HTTP_GET, [this](AsyncWebServerRequest *request)
                  { request->send(200, "application/json", HandleApiRequestGetWifiScanStatus()); });
    gWebServer.on("/api/settings_read", HTTP_GET, [this](AsyncWebServerRequest *request)
                  { request->send(200, "application/json", HandleApiRequestSettingsRead(request)); });
    gWebServer.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request)
                  { request->send(200, "application/json", HandleApiRequestStatusRead(request)); });
    gWebServer.onRequestBody([this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
                             {
                    if (request->url() == "/api/settings_write") {
                        request->send(200, "application/json", HandleApiRequestSettingsWrite(request, data));
                    } });

    gWebServer.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest *request)
                  { request->send(200, "application/json", HandleApiRequestReboot()); });
    gWebServer.on(
        "/api/update", HTTP_POST,
        [this](AsyncWebServerRequest *request)
        {
            if (Update.hasError())
            {
                request->send(400, "application/json", R"({"status": "error", "message": "Firmware update failed"})");
            }
            else
            {
                request->send(200, "application/json", R"({"status": "ok", "message": "Firmware updated successfully. Restarting..."})");
                delay(500); // Give the client time to receive the response
                ESP.restart();
            }
        },
        [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
        {
            if (!HandleFirmwareUpload(request, filename, index, data, len, final))
            {
                request->send(500, "application/json", R"({"status": "error", "message": "Upload handler failed"})");
            }
        });

    // handle 404 errors
    gWebServer.onNotFound([&](AsyncWebServerRequest *request)
                          { 
                            // Print all important request information
                            Logger::Debug("404: %s", request->url().c_str());
                            Logger::Debug(" > Method: %s", request->methodToString());
                            Logger::Debug(" > Content type: %s", request->contentType().c_str());
                            Logger::Debug(" > Content length: %d", request->contentLength());
                            Logger::Debug(" > Query string: %s", request->url().c_str());
                            request->send(404, "text/plain", "This resource does not exist"); });

    // Serve static files
    for (StaticFile file : staticFiles)
    {
        gWebServer.on(file.path, HTTP_GET, [this, file](AsyncWebServerRequest *request)
                      { mHadActivity = true;
                        request->send_P(200, file.contentType, file.data, file.size); });
    }

    gWebServer.begin();
}

// Handles the API request
String WebServer::HandleApiRequestGetVersion()
{
    Logger::Debug("Received REST API request: get version");

    JsonDocument doc;
    doc["version"] = String(Version::Major) + "." + String(Version::Minor) + "." + String(Version::Patch);
    doc["build_date"] = __DATE__;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    return jsonResponse;
}

// Handles the API request
String WebServer::HandleApiRequestStartWifiScan()
{
    Logger::Debug("Received REST API request: start WiFi scan");

    NetworkScanner::StartNetworkScan();

    JsonDocument doc;
    doc["status"] = "ok";

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    return jsonResponse;
}

// Handles the API request
String WebServer::HandleApiRequestGetWifiScanStatus()
{
    Logger::Debug("Received REST API request: get WiFi scan status");

    JsonDocument doc;
    NetworkScanner::GetNetworkScanResults(doc);

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    return jsonResponse;
}

// Handles the API request
String WebServer::HandleApiRequestSettingsRead(AsyncWebServerRequest *request)
{
    Logger::Debug("Received REST API request: read settings");

    // Return settings as JSON
    JsonDocument doc;

    doc["device-name"] = Settings::Instance()->DeviceName;
    doc["wifi-ssid"] = Settings::Instance()->WifiSsid;
    doc["wifi-password"] = Settings::Instance()->WifiPassword;
    doc["mqtt-enabled"] = Settings::Instance()->IsMqttEnabled;
    doc["mqtt-server"] = Settings::Instance()->MqttServer;
    doc["mqtt-port"] = Settings::Instance()->MqttPort;
    doc["mqtt-user"] = Settings::Instance()->MqttUser;
    doc["mqtt-password"] = Settings::Instance()->MqttPassword;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    return jsonResponse;
}

// Handles the API request
String WebServer::HandleApiRequestStatusRead(AsyncWebServerRequest *request)
{
    Logger::Debug("Received REST API request: read status");

    // Spannungen abrufen
    float v1 = 0, v2 = 0, v3 = 0;
    gWallbox->GetChargingVoltages(v1, v2, v3);

    float c1 = 0, c2 = 0, c3 = 0;
    gWallbox->GetChargingCurrents(c1, c2, c3);

    float p1 = v1 * c1;
    float p2 = v2 * c2;
    float p3 = v3 * c3;
    float pTotal = p1 + p2 + p3;

    // Return settings as JSON
    JsonDocument doc;

    JsonArray nrg = doc["nrg"].to<JsonArray>();
    nrg.add((int)v1); nrg.add((int)v2); nrg.add((int)v3); nrg.add(0);
    nrg.add((int)c1); nrg.add((int)c2); nrg.add((int)c3);
    nrg.add((int)p1); nrg.add((int)p2); nrg.add((int)p3); nrg.add(0);
    nrg.add((int)pTotal);
    nrg.add(1.0); nrg.add(1.0); nrg.add(1.0); nrg.add(0);

    float energyWh = gWallbox->GetEnergyMeterValue();
    doc["eto"] = (int)energyWh;
    doc["wh"] = (int)energyWh;
    doc["alw"] = (gWallbox->GetState() == VehicleState::Charging) ? 1 : 0;
    doc["amp"] = (int)(gWallbox->GetChargingCurrentLimit());
    doc["ama"] = (int)(gWallbox->GetFailsafeCurrent());

    doc["fwv"] = "4";
    doc["sse"] = "1234567";

    int car = 0;
    switch (gWallbox->GetState())
    {
        case VehicleState::Disconnected: car = 1; break;
        case VehicleState::Charging:     car = 2; break;
        case VehicleState::Connected:    car = 3; break;//waiting
        //case VehicleState::Charged:      car = 4; break;
        default:                         car = 0; break;
    }
    doc["car"] = car;

    doc["tmp"] = (int)(gWallbox->GetTemperature());

    JsonArray tma = doc["tma"].to<JsonArray>();
    tma.add((int)(gWallbox->GetTemperature()));

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    return jsonResponse;
}


// Handles the API request
String WebServer::HandleApiRequestSettingsWrite(AsyncWebServerRequest *request, uint8_t *data)
{
    Logger::Debug("Received REST API request: write settings");

    JsonDocument jsonBuffer;
    DeserializationError error = deserializeJson(jsonBuffer, (const char *)data);
    if (error)
    {
        Logger::Error("Failed to parse JSON");
        return R"({"status": "error", "message": "Failed to parse JSON"})";
    }
    JsonObject doc = jsonBuffer.as<JsonObject>();

    if (doc["device-name"].is<String>())
    {
        Settings::Instance()->DeviceName = doc["device-name"].as<String>();
    }
    if (doc["wifi-ssid"].is<String>())
    {
        Settings::Instance()->WifiSsid = doc["wifi-ssid"].as<String>();
    }
    if (doc["wifi-password"].is<String>())
    {
        Settings::Instance()->WifiPassword = doc["wifi-password"].as<String>();
    }
    if (doc["mqtt-enabled"].is<bool>())
    {
        Settings::Instance()->IsMqttEnabled = doc["mqtt-enabled"].as<bool>();
    }
    if (doc["mqtt-server"].is<String>())
    {
        Settings::Instance()->MqttServer = doc["mqtt-server"].as<String>();
    }
    if (doc["mqtt-port"].is<uint16_t>())
    {
        Settings::Instance()->MqttPort = doc["mqtt-port"].as<uint16_t>();
    }
    if (doc["mqtt-user"].is<String>())
    {
        Settings::Instance()->MqttUser = doc["mqtt-user"].as<String>();
    }
    if (doc["mqtt-password"].is<String>())
    {
        Settings::Instance()->MqttPassword = doc["mqtt-password"].as<String>();
    }

    Settings::Instance()->WriteToPersistentMemory();
    Settings::Instance()->Print();

    return R"({"status": "ok"})";
}

// Handles the API request
String WebServer::HandleApiRequestReboot()
{
    Logger::Info("Received REST API request: restarting ESP32");

    Settings::Instance()->DeInit();
    ESP.restart();

    return R"({"status": "ok"})";
}

// Handles firmware upload for OTA flashing
bool WebServer::HandleFirmwareUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0)
    {
        // First packet
        Logger::Info("Updating firmware: %s", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            Logger::Error("Starting firmware update failed: %s", Update.errorString());
            return false;
        }
    }

    if (Update.write(data, len) != len)
    {
        Logger::Error("Processing firmware update failed: %s", Update.errorString());
        return false;
    }

    if (final)
    {
        // Last packet
        if (Update.end(true))
        {
            Logger::Info("Firmware update complete");
        }
        else
        {
            Logger::Error("Finishing firmware update failed: %s", Update.errorString());
            return false;
        }
    }

    return true;
}

// Checks if any REST API calls have been made
bool WebServer::HadActivity()
{
    return mHadActivity;
}