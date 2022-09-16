#include <Arduino.h>
#include <DNSServer.h>              // part of ESP32 arduino core
#include <WiFi.h>                   // this as well
#include <ESPAsyncWebServer.h>      // 
#include <esp_wifi.h>

#include "chart.h"
#include "index_html.h"

// Change these to your desired flavors
const char *ssid = "Telemetry";      // Enter SSID here
const char *password = "12345678"; // Enter Password here

// Just any IPs will do really
IPAddress local_ip(10, 233, 239, 1);
IPAddress gateway(10, 233, 239, 1);
IPAddress subnet(255, 255, 255, 0);

DNSServer dnsServer;
AsyncWebServer webServer(80);

hw_timer_t * timer = NULL;

void handle_root(AsyncWebServerRequest *request);
void handle_chartjs(AsyncWebServerRequest *request);
void handle_not_found(AsyncWebServerRequest *request);

void setup()
{
    // begin timer
    timer = timerBegin(0, 80, true);

    Serial.begin(115200);

    WiFi.softAP(ssid, password);
    WiFi.softAPConfig(local_ip, gateway, subnet);

    delay(100);
    Serial.println("Access point started");

    webServer.on("/", handle_root);
    webServer.on("/chart-v3.9.1.min.js", handle_chartjs);
    webServer.on("/chart.js", handle_chartjs);
    webServer.onNotFound(handle_not_found);

    webServer.begin();
    Serial.println("HTTP server started");

    dnsServer.start(53, "*", local_ip);
    Serial.println("DNS server started");
}

void loop()
{
    dnsServer.processNextRequest();
    Serial.println(timerRead(timer));
    delay(1000);
}

void handle_root(AsyncWebServerRequest *request)
{
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html, index_html_length);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handle_chartjs(AsyncWebServerRequest *request)
{
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", chart_v3_9_1, chart_v3_9_1_length);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);    
}

void handle_not_found(AsyncWebServerRequest *request)
{
    Serial.println(request->host());
    if (request->host() != WiFi.softAPIP().toString())
    {
        request->redirect("http://" + WiFi.softAPIP().toString());
    }
    else
    {
        request->send(404);
    }
}
