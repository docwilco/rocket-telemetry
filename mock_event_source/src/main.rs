use std::sync::Arc;

use num_derive::FromPrimitive;
use rocket::{
    fs::NamedFile,
    get,
    response::stream::{Event, EventStream},
    routes,
    serde::Serialize,
    tokio::{
        self, select,
        sync::{
            broadcast::{channel, error::RecvError, Sender},
            RwLock,
        },
        time::{self, Duration, Instant},
    },
    FromFormField, Shutdown, State,
};

#[derive(Clone, Copy, FromFormField, FromPrimitive)]
enum AccelRange {
    #[field(value = "2")]
    Range2G = 2,
    #[field(value = "4")]
    Range4G = 4,
    #[field(value = "8")]
    Range8G = 8,
    #[field(value = "16")]
    Range16G = 16,
}

impl Default for AccelRange {
    fn default() -> Self {
        AccelRange::Range8G
    }
}

#[derive(Clone, Copy, FromFormField, FromPrimitive)]
enum GyroRange {
    #[field(value = "250")]
    Range250DPS = 250,
    #[field(value = "500")]
    Range500DPS = 500,
    #[field(value = "1000")]
    Range1000DPS = 1000,
    #[field(value = "2000")]
    Range2000DPS = 2000,
}

impl Default for GyroRange {
    fn default() -> Self {
        GyroRange::Range500DPS
    }
}

#[derive(Clone, Copy, FromFormField, FromPrimitive)]
enum FilterBandwidthHz {
    #[field(value = "260")]
    BandWidth260 = 260,
    #[field(value = "184")]
    BandWidth184 = 184,
    #[field(value = "94")]
    BandWidth94 = 94,
    #[field(value = "44")]
    BandWidth44 = 44,
    #[field(value = "21")]
    BandWidth21 = 21,
    #[field(value = "10")]
    BandWidth10 = 10,
    #[field(value = "5")]
    BandWidth5 = 5,
}

impl Default for FilterBandwidthHz {
    fn default() -> Self {
        FilterBandwidthHz::BandWidth21
    }
}

struct ServerState {
    telemetry_running: bool,
    telemetry_requested: bool,
    empty_weight: String,
    water_weight: String,
    air_pressure: String,
    accel_range: AccelRange,
    gyro_range: GyroRange,
    filter_bandwidth: FilterBandwidthHz,
    queue: Sender<Event>,
    event_id: u64,
}

impl ServerState {
    fn send_event(&mut self, event: Event) {
        self.event_id += 1;
        let id = format!("{}", self.event_id);
        // We don't care if there are no subscribers
        _ = self.queue.send(event.id(id));
    }
}

#[derive(Serialize)]
#[serde(crate = "rocket::serde")]
struct Parameters {
    empty_weight: String,
    water_weight: String,
    air_pressure: String,
    accel_range: u32,
    gyro_range: u32,
    filter_bandwidth: u32,
}

#[derive(Clone, Debug, Serialize)]
#[serde(crate = "rocket::serde")]
struct Telemetry {
    time: u64,
    acceleration_x: f32,
    acceleration_y: f32,
    acceleration_z: f32,
    #[serde(skip)]
    speed_z: f32,
    gyro_x: f32,
    gyro_y: f32,
    gyro_z: f32,
    pressure: f32,
    altitude: f32,
    bmp_temperature: f32,
    mpu_temperature: f32,
}

#[get("/")]
async fn index() -> Option<NamedFile> {
    NamedFile::open("../src/index.html").await.ok()
}

#[get("/chart-v3.9.1.min.js")]
async fn chartjs() -> Option<NamedFile> {
    NamedFile::open("../src/chart-v3.9.1.min.js").await.ok()
}

#[get("/chart.js")]
async fn chart() -> Option<NamedFile> {
    chartjs().await
}

#[get("/FileSaver-v2.0.5.min.js")]
async fn filesaver() -> Option<NamedFile> {
    NamedFile::open("../src/FileSaver-v2.0.5.min.js").await.ok()
}

#[get("/water-rocket.png")]
async fn favicon() -> Option<NamedFile> {
    NamedFile::open("../src/water-rocket.png").await.ok()
}

#[get("/start")]
async fn start(server_state: &State<Arc<RwLock<ServerState>>>) -> String {
    let mut server_state = server_state.write().await;
    if server_state.telemetry_requested {
        return "Telemetry already running".to_string();
    }
    server_state.telemetry_requested = true;
    "Telemetry started".to_string()
}

#[get("/stop")]
async fn stop(server_state: &State<Arc<RwLock<ServerState>>>) -> String {
    let mut server_state = server_state.write().await;
    if !server_state.telemetry_requested {
        return "Telemetry already stopped".to_string();
    }
    server_state.telemetry_requested = false;
    "Telemetry stopped".to_string()
}

#[get("/parameters?<empty_weight>&<water_weight>&<air_pressure>&<accel_range>&<gyro_range>&<filter_bandwidth>")]
async fn parameters(
    empty_weight: Option<String>,
    water_weight: Option<String>,
    air_pressure: Option<String>,
    accel_range: Option<AccelRange>,
    gyro_range: Option<GyroRange>,
    filter_bandwidth: Option<FilterBandwidthHz>,
    server_state: &State<Arc<RwLock<ServerState>>>,
) -> String {
    let mut server_state = server_state.write().await;
    if let Some(empty_weight) = empty_weight {
        server_state.empty_weight = empty_weight;
    }
    if let Some(water_weight) = water_weight {
        server_state.water_weight = water_weight;
    }
    if let Some(air_pressure) = air_pressure {
        server_state.air_pressure = air_pressure;
    }
    if let Some(accel_range) = accel_range {
        server_state.accel_range = accel_range;
    }
    if let Some(gyro_range) = gyro_range {
        server_state.gyro_range = gyro_range;
    }
    if let Some(filter_bandwidth) = filter_bandwidth {
        server_state.filter_bandwidth = filter_bandwidth;
    }
    let parameters = Parameters {
        empty_weight: server_state.empty_weight.clone(),
        water_weight: server_state.water_weight.clone(),
        air_pressure: server_state.air_pressure.clone(),
        accel_range: server_state.accel_range as u32,
        gyro_range: server_state.gyro_range as u32,
        filter_bandwidth: server_state.filter_bandwidth as u32,
    };
    server_state.send_event(Event::json(&parameters).event("parameters"));
    "".to_string()
}

#[get("/events")]
async fn events(
    server_state: &State<Arc<RwLock<ServerState>>>,
    mut shutdown: Shutdown,
) -> EventStream![] {
    let mut rx = server_state.read().await.queue.subscribe();
    EventStream! {
        loop {
            let event = select! {
                event = rx.recv() => match event {
                    Ok(event) => event,
                    Err(RecvError::Closed) => break,
                    Err(RecvError::Lagged(_)) => continue,
                },
                _ = &mut shutdown => break,
            };
            yield event;
        }
    }
}
async fn _jitter_around(value: &mut f32, plusminus: f32, center: f32) {
    *value = center + rand::random::<f32>() * plusminus 
}

async fn futz_with_telemetry(telemetry: &mut Telemetry, iterations: u64) {
    // Every iteration is 100ms. For the first second, just add a little jitter.
    // Then for one second, start with 4G acceleration and slowly decrease to -1G.
    // Update barometric pressure and height accordingly.
    // Then do nothing for 2 seconds.
    // Then simulate crashing into the ground.
    let time = iterations * 100; // Crude, but fine.
                                 // Set height (and barometric pressure) according to speed and acceleration.
                                 // Only if speed and altitude are not zero (or time is below 2000ms).
    if (telemetry.speed_z != 0.0 && telemetry.altitude != 0.0) || (time < 2000) {
        telemetry.altitude += telemetry.speed_z * 0.1;
        telemetry.speed_z += telemetry.acceleration_z * 0.1;
        if telemetry.altitude <= 0.0 && telemetry.speed_z <= 0.0 {
            telemetry.altitude = 0.0;
            telemetry.speed_z = 0.0;
        }
        telemetry.pressure = 101325.0 * (1.0 - 2.25577e-5 * telemetry.altitude).powf(5.25588);
    }
    telemetry.time = time;
    if time < 1000 {
    } else if time <= 2000 {
        let acceleration = 4.0 - 5.0 * (time - 1000) as f32 / 1000.0;
        // convert to m/s^2
        telemetry.acceleration_z = acceleration * 9.80665;
    }
    // Add a little jitter to the Z axis
    telemetry.acceleration_z += (rand::random::<f32>() - 0.5) * 0.5;
    println!("{telemetry:?}");
}

async fn generator_loop(server_state: Arc<RwLock<ServerState>>, mut shutdown: Shutdown) {
    let sleep = time::sleep(time::Duration::from_millis(100));
    tokio::pin!(sleep);
    let mut counter: u64 = 0;
    let mut start: u64 = 0;
    let tx = server_state.read().await.queue.clone();

    // Pressure at sea level is 1013.25 hPa. Set variable in Pascal.
    const START_TELEMETRY: Telemetry = Telemetry {
        time: 0,
        acceleration_x: 0.0,
        acceleration_y: 0.0,
        acceleration_z: -9.80665,
        speed_z: 0.0,
        gyro_x: 0.0,
        gyro_y: 0.0,
        gyro_z: 0.0,
        pressure: 101325.0,
        altitude: 0.0,
        bmp_temperature: 25.0,
        mpu_temperature: 24.9,
    };
    let mut telemetry = START_TELEMETRY;

    loop {
        select! {
            () = &mut sleep => {
                let mut events = Vec::new();
                {
                    let mut server_state = server_state.write().await;
                    counter += 1;
                    if server_state.telemetry_requested {
                        if !server_state.telemetry_running {
                            server_state.telemetry_running = true;
                            telemetry = START_TELEMETRY;
                            start = counter;
                            events.push(Event::empty().event("telemetry_started"));
                        }
                        futz_with_telemetry(&mut telemetry, counter - start).await;
                        events.push(Event::json(&telemetry).event("telemetry"));
                    } else {
                        if server_state.telemetry_running {
                            server_state.telemetry_running = false;
                            events.push(Event::empty().event("telemetry_stopped"));
                        }
                        if counter % 10 == 0 {
                            events.push(Event::empty().event("idle"));
                        }
                    }
                    for mut event in events {
                        event = event.id(format!("{}", server_state.event_id));
                        server_state.event_id += 1;
                        // If there are no receivers, that's fine.
                        let _ = tx.send(event);
                    }
                }
                sleep.as_mut().reset(Instant::now() + Duration::from_millis(100));
            },
            () = &mut shutdown => break,
        }
    }
}

#[rocket::main]
async fn main() -> Result<(), rocket::Error> {
    let (tx, _) = channel(100);
    let server_state = Arc::new(RwLock::new(ServerState {
        telemetry_running: false,
        telemetry_requested: false,
        empty_weight: String::from("0.0"),
        water_weight: String::from("0.0"),
        air_pressure: String::from("0.0"),
        accel_range: AccelRange::default(),
        gyro_range: GyroRange::default(),
        filter_bandwidth: FilterBandwidthHz::default(),
        queue: tx,
        event_id: 0,
    }));

    let state_for_generator = server_state.clone();
    let rocket = rocket::build()
        .mount(
            "/",
            routes![index, chart, chartjs, favicon, filesaver, events, start, stop, parameters],
        )
        .manage(server_state)
        .ignite()
        .await?;

    let shutdown = rocket.shutdown();
    tokio::spawn(generator_loop(state_for_generator, shutdown));

    let _result = rocket.launch().await?;
    Ok(())
}
