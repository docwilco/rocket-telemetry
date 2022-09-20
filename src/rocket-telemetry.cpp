#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>         // part of ESP32 arduino core
#include <WiFi.h>              // this as well
#include <ESPAsyncWebServer.h> //
#include <esp_wifi.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_BMP085.h>
#include <RingBuf.h>
#include <vector>
#include "chart_v3_9_1_min_js.h"
#include "index_html.h"
#include "FileSaver_v2_0_5_min_js.h"

// Change these to your desired flavors
const char *ssid = "Telemetry";    // Enter SSID here
const char *password = "12345678"; // Enter Password here

// Just any IPs will do really
IPAddress local_ip(10, 233, 239, 1);
IPAddress gateway(10, 233, 239, 1);
IPAddress subnet(255, 255, 255, 0);

DNSServer dnsServer;
AsyncWebServer webServer(80);
AsyncEventSource events("/events");
std::vector<AsyncEventSourceClient *> clients;

hw_timer_t *timer = NULL;
Adafruit_BMP085 bmp;
Adafruit_MPU6050 mpu;

volatile bool telemetry_running = false;
volatile bool calibrate = false;
float zero_pressure = 101325; // standard atmospheric pressure in Pa
uint64_t event_id;

void handle_root(AsyncWebServerRequest *request);
void handle_chartjs(AsyncWebServerRequest *request);
void handle_filesaver(AsyncWebServerRequest *request);
void handle_not_found(AsyncWebServerRequest *request);
void handle_start(AsyncWebServerRequest *request);
void handle_stop(AsyncWebServerRequest *request);
void handle_calibrate(AsyncWebServerRequest *request);
void init_sensors();
void do_telemetry();

void setup()
{

    Serial.begin(115200);

    init_sensors();

    Serial.println("");
    WiFi.softAP(ssid, password);
    WiFi.softAPConfig(local_ip, gateway, subnet);

    delay(100);
    Serial.println("Access point started");

    webServer.on("/", handle_root);
    webServer.on("/chart-v3.9.1.min.js", handle_chartjs);
    webServer.on("/FileSaver-v2.0.5.min.js", handle_filesaver);
    webServer.on("/chart.js", handle_chartjs);
    webServer.on("/start", handle_start);
    webServer.on("/stop", handle_stop);
    webServer.on("/calibrate", handle_calibrate);
    webServer.onNotFound(handle_not_found);
    events.onConnect([](AsyncEventSourceClient *client)
                     {
        if (client->lastId())
        {
            Serial.printf("Client reconnected! Last message ID that it got is: %u\n", client->lastId());
        }
        clients.push_back(client); });
    webServer.addHandler(&events);
    webServer.begin();
    Serial.println("HTTP server started");

    dnsServer.start(53, "*", local_ip);
    Serial.println("DNS server started");
}

void loop()
{
    dnsServer.processNextRequest();
    if (calibrate) {
        zero_pressure = bmp.readPressure();
        calibrate = false;
    }
    if (telemetry_running)
    {
        do_telemetry();
    } else {
        delay(100);
    }
}

void do_telemetry()
{
    unsigned long read_sensor_start = micros();
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float pressure = bmp.readPressure();
    float altitude = bmp.readAltitude(zero_pressure);
    float temperature = bmp.readTemperature();
    unsigned long read_sensor_end = micros();
    unsigned long read_sensor_duration = read_sensor_end - read_sensor_start;
    unsigned long make_json_start = micros();
    const int capacity = JSON_OBJECT_SIZE(11);
    StaticJsonDocument<capacity> json;
    json["time"] = timerReadMilis(timer);
    json["acceleration_x"] = a.acceleration.x;
    json["acceleration_y"] = a.acceleration.y;
    json["acceleration_z"] = a.acceleration.z;
    json["gyro_x"] = g.gyro.x;
    json["gyro_y"] = g.gyro.y;
    json["gyro_z"] = g.gyro.z;
    json["pressure"] = pressure;
    json["altitude"] = altitude;
    json["bmp_temperature"] = temperature;
    json["mpu_temperature"] = temp.temperature;
    String json_string = "";
    serializeJson(json, json_string);
    unsigned long make_json_end = micros();
    unsigned long make_json_duration = make_json_end - make_json_start;
    event_id++;
    // use remove_if to iterate over the clients and
    // send the data to all of them, removing the ones
    // that are disconnected
    unsigned long send_event_start = micros();
    clients.erase(
        std::remove_if(clients.begin(), clients.end(),
                       [&](AsyncEventSourceClient *client)
                       {
                           if (!client->connected())
                           {
                               return true;
                           }
                           client->send(json_string.c_str(), "telemetry", event_id);
                           return false;
                       }),
        clients.end());
    unsigned long send_event_end = micros();
    unsigned long send_event_duration = send_event_end - send_event_start;
    //Serial.printf("Read sensor: %lu us, make json: %lu us, send event: %lu us\n", read_sensor_duration, make_json_duration, send_event_duration);
}

void handle_file(AsyncWebServerRequest *request, const String &content_type, const uint8_t *data, size_t data_length)
{
    AsyncWebServerResponse *response = request->beginResponse_P(200, content_type, data, data_length);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handle_root(AsyncWebServerRequest *request)
{
    if (request->host() != WiFi.softAPIP().toString())
    {
        request->redirect("http://" + WiFi.softAPIP().toString());
    }
    else
    {
        handle_file(request, "text/html", index_html, index_html_length);
    }
}

void handle_chartjs(AsyncWebServerRequest *request)
{
    handle_file(request, "text/javascript", chart_v3_9_1_min_js, chart_v3_9_1_min_js_length);
}

void handle_filesaver(AsyncWebServerRequest *request)
{
    handle_file(request, "text/javascript", FileSaver_v2_0_5_min_js, FileSaver_v2_0_5_min_js_length);
}

void handle_start(AsyncWebServerRequest *request)
{
    if (telemetry_running)
    {
        request->send(200, "text/plain", "Telemetry already running");
        return;
    }
    Serial.println("Starting telemetry");
    event_id = 0;
    timer = timerBegin(0, 80, true);
    telemetry_running = true;
    request->send(200, "text/plain", "Telemetry started");
}

void handle_stop(AsyncWebServerRequest *request)
{
    if (!telemetry_running)
    {
        request->send(200, "text/plain", "Telemetry already stopped");
        return;
    }
    Serial.println("Stopping telemetry");
    telemetry_running = false;
    // close all clients
    for (auto client : clients)
    {
        client->close();
    }
    clients.clear();
    request->send(200, "text/plain", "Telemetry stopped");
}

void handle_calibrate(AsyncWebServerRequest *request)
{
    request->send(200, "text/plain", "Calibration done");
}

void handle_not_found(AsyncWebServerRequest *request)
{
    if (request->host() != WiFi.softAPIP().toString())
    {
        request->redirect("http://" + WiFi.softAPIP().toString());
    }
    else
    {
        request->send(404);
    }
}

void init_sensors()
{
    if (!bmp.begin())
    {
        Serial.println("Could not find a valid BMP085 sensor, check wiring!");
        while (1)
        {
            delay(1000);
        }
    }
    else
    {
        Serial.println("BMP085 sensor found");
    }

    if (!mpu.begin())
    {
        Serial.println("Failed to find MPU6050 chip");
        while (1)
        {
            delay(1000);
        }
    }
    else
    {
        Serial.println("MPU6050 sensor found");
    }
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    Serial.print("Accelerometer range set to: ");
    switch (mpu.getAccelerometerRange())
    {
    case MPU6050_RANGE_2_G:
        Serial.println("+-2G");
        break;
    case MPU6050_RANGE_4_G:
        Serial.println("+-4G");
        break;
    case MPU6050_RANGE_8_G:
        Serial.println("+-8G");
        break;
    case MPU6050_RANGE_16_G:
        Serial.println("+-16G");
        break;
    }
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    Serial.print("Gyro range set to: ");
    switch (mpu.getGyroRange())
    {
    case MPU6050_RANGE_250_DEG:
        Serial.println("+- 250 deg/s");
        break;
    case MPU6050_RANGE_500_DEG:
        Serial.println("+- 500 deg/s");
        break;
    case MPU6050_RANGE_1000_DEG:
        Serial.println("+- 1000 deg/s");
        break;
    case MPU6050_RANGE_2000_DEG:
        Serial.println("+- 2000 deg/s");
        break;
    }

    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.print("Filter bandwidth set to: ");
    switch (mpu.getFilterBandwidth())
    {
    case MPU6050_BAND_260_HZ:
        Serial.println("260 Hz");
        break;
    case MPU6050_BAND_184_HZ:
        Serial.println("184 Hz");
        break;
    case MPU6050_BAND_94_HZ:
        Serial.println("94 Hz");
        break;
    case MPU6050_BAND_44_HZ:
        Serial.println("44 Hz");
        break;
    case MPU6050_BAND_21_HZ:
        Serial.println("21 Hz");
        break;
    case MPU6050_BAND_10_HZ:
        Serial.println("10 Hz");
        break;
    case MPU6050_BAND_5_HZ:
        Serial.println("5 Hz");
        break;
    }
}