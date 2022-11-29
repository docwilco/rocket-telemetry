#include <Adafruit_BMP085.h>
#include <Adafruit_MPU6050.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>          // part of ESP32 arduino core
#include <ESPAsyncWebServer.h>  //
#include <EasyButton.h>
#include <RingBuf.h>
#include <TFT_eSPI.h>
#include <WiFi.h>  // this as well
#include <esp_wifi.h>

#include <vector>

#include "FileSaver_v2_0_5_min_js.h"
#include "chart_v3_9_1_min_js.h"
#include "index_html.h"
#include "nyancat_bmp.h"

// Change these to your desired flavors
const char *ssid = "Telemetry";     // Enter SSID here
const char *password = "12345678";  // Enter Password here

// Just any IPs will do really
IPAddress local_ip(10, 233, 239, 1);
IPAddress gateway(10, 233, 239, 1);
IPAddress subnet(255, 255, 255, 0);

#define ADC_EN 14  // ADC_EN is the ADC detection enable port
#define ADC_PIN 34
#define BUTTON_1 35
#define BUTTON_2 0
#define BACKLIGHT_PIN 4
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 135
#define BUTTON_WIDTH 80
#define BUTTON_HEIGHT 40

DNSServer dnsServer;
AsyncWebServer webServer(80);
AsyncEventSource events("/events");
std::vector<AsyncEventSourceClient *> clients;

hw_timer_t *timer = NULL;
Adafruit_BMP085 bmp;
Adafruit_MPU6050 mpu;
TFT_eSPI tft = TFT_eSPI();
EasyButton button1(BUTTON_1);
EasyButton button2(BUTTON_2);

volatile bool telemetry_requested = false;
bool telemetry_running = false;
volatile bool calibration_requested = false;
float zero_pressure = 101325;  // standard atmospheric pressure in Pa
float max_altitude = NAN;
float prev_max_altitude = NAN;
float max_z_accel = NAN;
float prev_max_z_accel = NAN;
float prev_battery_voltage = NAN;
float prev_temperature = NAN;
uint64_t event_id = 0;
uint32_t loop_counter = 0;
volatile bool backlight_on = true;  // it is on by default
bool backlight_requested = true;

void handle_root(AsyncWebServerRequest *request);
void handle_chartjs(AsyncWebServerRequest *request);
void handle_filesaver(AsyncWebServerRequest *request);
void handle_not_found(AsyncWebServerRequest *request);
void handle_start(AsyncWebServerRequest *request);
void handle_stop(AsyncWebServerRequest *request);
void handle_calibrate(AsyncWebServerRequest *request);
void init_sensors();
void do_telemetry();
void do_idle();
void send_event(const char *message, const char *event);
void draw_grid();
void draw_button_labels();
void draw_telemetry();
float calc_battery_voltage();

void button1_ISR() { button1.read(); }
void button2_ISR() { button2.read(); }

void setup() {
    Serial.begin(115200);

    button1.begin();
    button2.begin();
    if (!button1.supportsInterrupt()) {
        Serial.println("Button 1 does not support interrupts");
        while (true) {
        }
    }
    if (!button2.supportsInterrupt()) {
        Serial.println("Button 2 does not support interrupts");
        while (true) {
        }
    }
    button1.enableInterrupt(button1_ISR);
    button2.enableInterrupt(button2_ISR);
    button1.onPressed([]() {
        Serial.println("Button 1 pressed");
        calibration_requested = true;
        telemetry_requested = !telemetry_requested;
    });
    button2.onPressed([]() {
        Serial.println("Button 2 pressed");
        backlight_requested = !backlight_requested;
    });
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(0, 0);
    draw_grid();
    draw_button_labels();

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

    // flip backlight if needed.
    // don't do anything until after a few seconds, to
    // allow the backlight button label to be seen, since
    // the default is to turn the backlight off.
    if (loop_counter > 20 && backlight_on != backlight_requested) {
        digitalWrite(BACKLIGHT_PIN, backlight_requested ? HIGH : LOW);
        backlight_on = backlight_requested;
    }

    if (calibration_requested) {
        Serial.println("Calibration requested");
        zero_pressure = bmp.readPressure();
        Serial.printf("Zero pressure: %f Pa\n", zero_pressure);
        calibration_requested = false;
    }
    if (telemetry_requested) {
        do_telemetry();
    } else {
        do_idle();
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
    if (!telemetry_running) {
        Serial.println("Starting telemetry");
        telemetry_running = true;
        assert(timer == NULL);
        timer = timerBegin(0, 80, true);
        // like the _stopped event, we don't want any idle events
        // after the _started event. So we send it here instead of
        // handle_start.
        send_event(NULL, "telemetry_started");
        int rotation = tft.getRotation();
        tft.setRotation(7);
        tft.pushImage(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, nyancat_bmp);
        delay(2000);
        tft.setRotation(rotation);
        tft.fillScreen(TFT_BLACK);
        max_altitude = NAN;
        max_z_accel = NAN;
        prev_max_altitude = NAN;
        prev_max_z_accel = NAN;
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

    // update max_altitude if higher or if max is NAN
    if (isnan(max_altitude) || altitude > max_altitude) {
        max_altitude = altitude;
    }
    // update max_z_accel if higher or if max is NAN
    if (isnan(max_z_accel) || a.acceleration.z > max_z_accel) {
        max_z_accel = a.acceleration.z;
    }

    if (max_altitude != prev_max_altitude || max_z_accel != prev_max_z_accel) {
        static char buf[100] = "";
        static char prev_buf[100] = "";
        // format the string to display
        sprintf(buf, "Max altitude:\n%.2f m\n\nMax z accel:\n%.2f m/s^2",
                max_altitude, max_z_accel);
        if (strcmp(buf, prev_buf) != 0) {
            // only update the screen if the string has changed
            tft.fillScreen(TFT_BLACK);
            tft.setCursor(0, 0);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.setTextSize(3);
            tft.println(buf);
            strcpy(prev_buf, buf);
        }
    }
}

void do_idle() {
    static char buf[100] = "";
    static char prev_buf[100] = "";
    if (telemetry_running) {
        telemetry_running = false;
        assert(timer != NULL);
        Serial.println("Stopping telemetry");
        timerEnd(timer);
        timer = NULL;
        // sending event here instead of handle_stop because we don't
        // want any telemetry events after the _stopped event. Which
        // would happen if we sent the event in handle_stop.
        send_event(NULL, "telemetry_stopped");
        tft.fillScreen(TFT_BLACK);
        prev_temperature = NAN;
        prev_battery_voltage = NAN;
        prev_buf[0] = '\0';
    }
    // only send idle events or do display updates every 10 loops
    if (loop_counter % 10 == 0) {
        send_event(NULL, "idle");
        // update the temperature and battery readings if they have changed
        float temperature = bmp.readTemperature();
        float battery_voltage = calc_battery_voltage();
        if (temperature != prev_temperature ||
            battery_voltage != prev_battery_voltage) {
            // format the string to display
            sprintf(buf, "Temp:\n%.1f C\n\nBattery:\n%.2f V",
                    temperature, battery_voltage);
            if (strcmp(buf, prev_buf) != 0) {
                // only update the screen if the string has changed
                tft.fillScreen(TFT_BLACK);
                tft.setCursor(0, 0);
                tft.setTextColor(TFT_WHITE, TFT_BLACK);
                tft.setTextSize(3);
                tft.println(buf);
                tft.setTextSize(2);
                tft.setCursor(76, 19);
                tft.println("o");
                strcpy(prev_buf, buf);
                draw_button_labels();
            }
        }
    }
    delay(100);
}

void draw_button_labels() {
    // white boxes in top and bottom right corners
    tft.fillRect(SCREEN_WIDTH - BUTTON_WIDTH, 0, BUTTON_WIDTH, BUTTON_HEIGHT,
                 TFT_WHITE);
    tft.fillRect(SCREEN_WIDTH - BUTTON_WIDTH, SCREEN_HEIGHT - BUTTON_HEIGHT,
                 BUTTON_WIDTH, BUTTON_HEIGHT, TFT_WHITE);

    // Use Middle Center datum to draw text in middle of box
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(telemetry_running ? "Stop" : "Start",
                   SCREEN_WIDTH - BUTTON_WIDTH / 2, BUTTON_HEIGHT / 2);
    tft.drawString("Screen", SCREEN_WIDTH - BUTTON_WIDTH / 2,
                   SCREEN_HEIGHT - BUTTON_HEIGHT / 2);
}

void draw_grid() {
#define GRID_STEP 8
#define MINOR_GRID tft.color565(150, 150, 150)
#define MAJOR_GRID tft.color565(180, 180, 180)
#define GRID_EDGE tft.color565(255, 0, 0)
#define V_CENTER tft.color565(255, 0, 0)
#define H_CENTER tft.color565(255, 0, 0)

    // draw minor lines first so that the major lines overlap them on the
    // cross-sections
    for (int v = (GRID_STEP / 2) - 1; v < SCREEN_WIDTH; v += GRID_STEP) {
        // minor
        tft.drawFastVLine(v, 0, SCREEN_HEIGHT, MINOR_GRID);
    }
    for (int h = (GRID_STEP / 2) - 1; h < SCREEN_HEIGHT; h += GRID_STEP) {
        // minor
        tft.drawFastHLine(0, h, SCREEN_WIDTH, MINOR_GRID);
    }

    // next major lines, overlapping the minor lines at cross-sections
    for (int v = GRID_STEP - 1; v < SCREEN_WIDTH; v += GRID_STEP) {
        // main
        tft.drawFastVLine(v, 0, SCREEN_HEIGHT, MAJOR_GRID);
    }
    for (int h = GRID_STEP - 1; h < SCREEN_HEIGHT; h += GRID_STEP) {
        // main:
        tft.drawFastHLine(0, h, SCREEN_WIDTH, MAJOR_GRID);
    }
    // edge lines
    // tft.drawFastVLine(0, 0, SCREEN_HEIGHT - 1, GRID_EDGE);
    // tft.drawFastVLine(SCREEN_WIDTH - 1, 0, SCREEN_HEIGHT - 1, GRID_EDGE);
    // tft.drawFastHLine(0, 0, SCREEN_WIDTH - 1, GRID_EDGE);
    // tft.drawFastHLine(0, SCREEN_HEIGHT - 1, SCREEN_HEIGHT - 1, GRID_EDGE);
    // center lines
    tft.drawFastVLine(SCREEN_WIDTH / 2 - 1, 0, SCREEN_HEIGHT - 1, V_CENTER);
    tft.drawFastHLine(0, SCREEN_HEIGHT / 2 - 1, SCREEN_WIDTH - 1, V_CENTER);
}

float calc_battery_voltage() {
    // uint16_t v = analogRead(ADC_PIN);
    // int vref = 1100;
    // float ret = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);

    // The above is the way the demo software at
    // https://github.com/Xinyuan-LilyGO/TTGO-T-Display/blob/master/TFT_eSPI/examples/FactoryTest/FactoryTest.ino
    // does it. We can simplify that to:
    return analogRead(ADC_PIN) * 0.00177289377;
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