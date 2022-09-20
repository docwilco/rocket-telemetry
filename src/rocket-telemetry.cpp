#include <Adafruit_BMP085.h>
#include <Adafruit_MPU6050.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>          // part of ESP32 arduino core
#include <ESPAsyncWebServer.h>  //
#include <RingBuf.h>
#include <WiFi.h>  // this as well
#include <esp_wifi.h>

#include <vector>

#include "FileSaver_v2_0_5_min_js.h"
#include "chart_v3_9_1_min_js.h"
#include "index_html.h"

// Change these to your desired flavors
const char *ssid = "Telemetry";     // Enter SSID here
const char *password = "12345678";  // Enter Password here

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

volatile bool telemetry_requested = false;
volatile bool calibration_requested = false;
float zero_pressure = 101325;  // standard atmospheric pressure in Pa
uint64_t event_id = 0;
uint32_t loop_counter = 0;

void handle_root(AsyncWebServerRequest *request);
void handle_chartjs(AsyncWebServerRequest *request);
void handle_filesaver(AsyncWebServerRequest *request);
void handle_not_found(AsyncWebServerRequest *request);
void handle_start(AsyncWebServerRequest *request);
void handle_stop(AsyncWebServerRequest *request);
void handle_calibrate(AsyncWebServerRequest *request);
void init_sensors();
void do_telemetry();
void send_event(const char *message, const char *event);

void setup() {
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
    events.onConnect([](AsyncEventSourceClient *client) {
        if (client->lastId()) {
            Serial.printf(
                "Client reconnected! Last message ID that it got is: %u\n",
                client->lastId());
        }
        clients.push_back(client);
    });
    webServer.addHandler(&events);
    webServer.begin();
    Serial.println("HTTP server started");

    dnsServer.start(53, "*", local_ip);
    Serial.println("DNS server started");
}

void loop() {
    loop_counter++;
    dnsServer.processNextRequest();
    if (calibration_requested) {
        Serial.println("Calibration requested");
        zero_pressure = bmp.readPressure();
        Serial.printf("Zero pressure: %f Pa\n", zero_pressure);
        calibration_requested = false;
    }
    if (telemetry_requested) {
        do_telemetry();
    } else {
        // we're basically using the timer state to determine if we were
        // running telemetry or not until now.
        if (timer != NULL) {
            Serial.println("Stopping timer");
            timerEnd(timer);
            timer = NULL;
            // sending event here instead of handle_stop because we don't
            // want any telemetry events after the _stopped event. Which
            // would happen if we sent the event in handle_stop.
            send_event(NULL, "telemetry_stopped");
        }
        // only send idle events every 10 loops
        if (loop_counter % 10 == 0) {
            send_event(NULL, "idle");
        }
        delay(100);
    }
}

void send_event(const char *message, const char *event) {
    event_id++;
    // use remove_if to iterate over the clients and
    // send the data to all of them, removing the ones
    // that are disconnected
    const char *message_to_send;
    if (message == NULL) {
        // Serial.printf("Sending event %s\n", event);
        message_to_send = "";
    } else {
        // Serial.printf("Sending event %s: %s\n", event, message);
        message_to_send = message;
    }
    clients.erase(std::remove_if(clients.begin(), clients.end(),
                                 [&](AsyncEventSourceClient *client) {
                                     if (!client->connected() ||
                                         client->packetsWaiting() >=
                                             SSE_MAX_QUEUED_MESSAGES) {
                                         return true;
                                     }
                                     client->send(message_to_send, event,
                                                  event_id);
                                     return false;
                                 }),
                  clients.end());
}

void do_telemetry() {
    if (timer == NULL) {
        timer = timerBegin(0, 80, true);
        // like the _stopped event, we don't want any idle events
        // after the _started event. So we send it here instead of
        // handle_start.
        send_event(NULL, "telemetry_started");
    }
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
    unsigned long send_event_start = micros();
    send_event(json_string.c_str(), "telemetry");
    unsigned long send_event_end = micros();
    unsigned long send_event_duration = send_event_end - send_event_start;
    // Serial.printf("Read sensor: %lu us, make json: %lu us, send event: %lu
    // us\n", read_sensor_duration, make_json_duration, send_event_duration);
}

void handle_file(AsyncWebServerRequest *request, const String &content_type,
                 const uint8_t *data, size_t data_length) {
    AsyncWebServerResponse *response =
        request->beginResponse_P(200, content_type, data, data_length);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
}

void handle_root(AsyncWebServerRequest *request) {
    if (request->host() != WiFi.softAPIP().toString()) {
        request->redirect("http://" + WiFi.softAPIP().toString());
    } else {
        handle_file(request, "text/html", index_html, index_html_length);
    }
}

void handle_chartjs(AsyncWebServerRequest *request) {
    handle_file(request, "text/javascript", chart_v3_9_1_min_js,
                chart_v3_9_1_min_js_length);
}

void handle_filesaver(AsyncWebServerRequest *request) {
    handle_file(request, "text/javascript", FileSaver_v2_0_5_min_js,
                FileSaver_v2_0_5_min_js_length);
}

void handle_start(AsyncWebServerRequest *request) {
    if (telemetry_requested) {
        request->send(200, "text/plain", "Telemetry already running");
        return;
    }
    Serial.println("Starting telemetry");
    telemetry_requested = true;
    request->send(200, "text/plain", "Telemetry started");
}

void handle_stop(AsyncWebServerRequest *request) {
    if (!telemetry_requested) {
        request->send(200, "text/plain", "Telemetry already stopped");
        return;
    }
    Serial.println("Stopping telemetry");
    telemetry_requested = false;
    request->send(200, "text/plain", "Telemetry stopped");
}

void handle_calibrate(AsyncWebServerRequest *request) {
    calibration_requested = true;
    request->send(200, "text/plain", "Calibration done");
}

void handle_not_found(AsyncWebServerRequest *request) {
    if (request->host() != WiFi.softAPIP().toString()) {
        request->redirect("http://" + WiFi.softAPIP().toString());
    } else {
        request->send(404);
    }
}

void init_sensors() {
    if (!bmp.begin()) {
        Serial.println("Could not find a valid BMP085 sensor, check wiring!");
        while (1) {
            delay(1000);
        }
    } else {
        Serial.println("BMP085 sensor found");
    }

    if (!mpu.begin()) {
        Serial.println("Failed to find MPU6050 chip");
        while (1) {
            delay(1000);
        }
    } else {
        Serial.println("MPU6050 sensor found");
    }
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    Serial.print("Accelerometer range set to: ");
    switch (mpu.getAccelerometerRange()) {
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
    switch (mpu.getGyroRange()) {
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
    switch (mpu.getFilterBandwidth()) {
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