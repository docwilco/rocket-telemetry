# Water Rocket Telemetry

This is firmware for an ESP32 based telemetry system for water rockets. It has support for the TTGO T-Display, but works with a regular ESP32 as well. (Or will, soon O:) )

The way it works is it starts a WiFi Access Point that you can connect to with a phone, tablet, or laptop. When you connect to said WiFi, you will be shown a webpage with the telemetry data. It will save multiple runs for comparison.

You can connect with multiple devices at once, and it will show the telemetry data on all of them, and controls are shared as well.

## Sensors

This current incarnation is meant for the GY-88A breakout board, which has a BMP085 (barometric sensor), a MPU6050 (accelerometer and gyroscope), and a HMC5883L (magnetometer). The latter is not used.

Adapting the code to use different sensors should be fairly easy, as very little code is specific to reading the sensors.

## Building and flashing

This is a [PlatformIO](https://platformio.org/) project, so it as simple as [installing PlatformIO](https://platformio.org/install) (I would recommend the IDE option, but the CLI is fine as well) and either opening the project in the IDE and clicking the upload button, or running `platformio run -t upload` in the project directory.

## Development

The `mock_event_source` directory contains a Rust project that emulates the telemetry server. It is useful for testing the web interface without having to flash the firmware and connect to the ESP32 WiFi. To run it, simply [install Rust](https://www.rust-lang.org/tools/install) and run `cargo run` in the directory. Note that by default, it will bind to 0.0.0.0:8000. This means that it will be accessible from other devices on your network. If you want to run it on your local machine only, edit the `Rocket.toml` file and comment out `address`.

The static files it serves are not cached, so a reload in the browser after updating a file is all that is needed. Of course, if any of the mock server's code is changed, a rebuild and restart is needed. You can automate this by running `cargo watch -x run` instead of `cargo run`. If you don't have `cargo watch` installed, you can install it with `cargo install cargo-watch`.

The fact that the mock server uses the [Rocket framework](https://rocket.rs/) is purely coincidental, but fitting.


## Credits

All of the code is written by Rogier "DocWilco" Mulhuijzen.

The favicon is [Water rocket icon created by Freepik - Flaticon](https://www.flaticon.com/free-icons/water-rocket)
