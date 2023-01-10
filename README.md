# Water Rocket Telemetry

This is firmware for an ESP32 based telemetry system for water rockets. It has support for the TTGO T-Display, but works with a regular ESP32 as well. (Or will, soon O:) )

The way it works is it starts a WiFi Access Point that you can connect to with a phone, tablet, or laptop. When you connect to said WiFi, you will be shown a webpage with the telemetry data. It will save multiple runs for comparison.

You can connect with multiple devices at once, and it will show the telemetry data on all of them, and controls are shared as well.

## Sensors

This current incarnation is meant for the GY-88A breakout board, which has a BMP085 (barometric sensor), a MPU6050 (accelerometer and gyroscope), and a HMC5883L (magnetometer). The latter is not used.

Adapting the code to use different sensors should be fairly easy, as very little code is specific to reading the sensors.

## Building

This is a [PlatformIO](https://platformio.org/) project, so you can just open it in VSCode and build it.

## Development

The `mock_event_source` directory contains a Rust project that uses Rocket and emulates the telemetry server. It is useful for testing the web interface without having to deploy the firmware and connect to it.

## Credits

All of the code is written by Rogier "DocWilco" Mulhuijzen.

The favicon is [Water rocket icon created by Freepik - Flaticon](https://www.flaticon.com/free-icons/water-rocket)
