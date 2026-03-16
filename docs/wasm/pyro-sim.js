/**
 * Pyro MK1B Simulation — ES Module Wrapper
 *
 * Clean JavaScript API around the WASM flight computer.
 * Import this module from any web project to drive the pyro
 * flight computer as a black box.
 *
 * Usage:
 *   import { createPyroSim } from './pyro-sim.js';
 *   const sim = await createPyroSim();
 *   sim.init("pyro1_mode=delay\npyro1_value=0\nunits=ft\n");
 *   sim.setPressure(101325);
 *   const state = sim.tick(0);
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * Load the WASM module and return a high-level sim object.
 * @param {string} [wasmPath] - Path to pyro.js (Emscripten glue). Defaults to same directory.
 * @returns {Promise<PyroSim>}
 */
export async function createPyroSim(wasmPath) {
    const base = wasmPath || new URL('./pyro.js', import.meta.url).href;

    // Load the Emscripten module
    const Module = await new Promise((resolve, reject) => {
        const script = document.createElement('script');
        script.src = base;
        script.onload = () => {
            // Emscripten sets Module globally or on window
            const mod = window.Module;
            if (mod && mod.onRuntimeInitialized) {
                const orig = mod.onRuntimeInitialized;
                mod.onRuntimeInitialized = () => { orig(); resolve(mod); };
            } else if (mod && mod.calledRun) {
                resolve(mod);
            } else {
                // Wait for initialization
                const check = setInterval(() => {
                    if (window.Module && window.Module.calledRun) {
                        clearInterval(check);
                        resolve(window.Module);
                    }
                }, 10);
                setTimeout(() => { clearInterval(check); reject(new Error('WASM load timeout')); }, 10000);
            }
        };
        script.onerror = () => reject(new Error('Failed to load ' + base));
        document.head.appendChild(script);
    });

    return new PyroSim(Module);
}

/**
 * If the Emscripten Module is already loaded (e.g. via <script> tag),
 * wrap it directly.
 * @param {object} Module - The Emscripten Module object
 * @returns {PyroSim}
 */
export function wrapModule(Module) {
    return new PyroSim(Module);
}

/** Flight state enum */
export const FlightState = Object.freeze({
    BOOT_INIT: 0,
    BOOT_SETTLE: 1,
    BOOT_CONTINUITY: 2,
    BOOT_CALIBRATE: 3,
    PAD_IDLE: 4,
    ASCENT: 5,
    DESCENT: 6,
    LANDED: 7,
});

/** State name lookup */
export const STATE_NAMES = [
    'BOOT_INIT', 'BOOT_SETTLE', 'BOOT_CONTINUITY', 'BOOT_CALIBRATE',
    'PAD_IDLE', 'ASCENT', 'DESCENT', 'LANDED'
];

/**
 * High-level simulation API.
 * All methods map 1:1 to the C pyro_sim.h functions.
 */
class PyroSim {
    constructor(mod) {
        this._m = mod;
        this._physics = new PhysicsEngine(mod);
    }

    /** Access the physics engine */
    get physics() { return this._physics; }

    /** Initialize the flight computer with an optional config.ini string */
    init(configIni) {
        if (configIni) {
            const buf = this._m._malloc(configIni.length + 1);
            this._m.stringToUTF8(configIni, buf, configIni.length + 1);
            this._m._sim_flight_init(buf);
            this._m._free(buf);
        } else {
            this._m._sim_flight_init(0);
        }
    }

    /** Advance one tick. Returns the current flight state (0-7). */
    tick(timeMs) { return this._m._sim_flight_tick(timeMs); }

    /** Reset all state to power-on defaults */
    reset() { this._m._sim_reset(); }

    // ── Inputs ──

    /** Set simulation time (ms) */
    setTime(ms) { this._m._sim_set_time(ms); }

    /** Set barometric pressure (Pascals) */
    setPressure(pa) { this._m._sim_set_pressure(pa); }

    /** Set sensor type (0=none, 1=ms5607, 2=bmp280) */
    setSensorType(type) { this._m._sim_set_sensor_type(type); }

    /** Set pyro continuity: ch=1|2, adc=raw, good=bool, open=bool */
    setContinuity(ch, adc, good, open) {
        this._m._sim_set_continuity(ch, adc, good ? 1 : 0, open ? 1 : 0);
    }

    /** Clear pyro firing flag (call before each tick) */
    clearPyroFiring() { this._m._sim_clear_pyro_firing(); }

    // ── Outputs ──

    /** Current flight state (0-7) */
    get state() { return this._m._sim_flight_state(); }

    /** State name string */
    get stateName() { return STATE_NAMES[this.state] || '?'; }

    /** Filtered altitude in centimeters */
    get altitudeCm() { return this._m._sim_flight_altitude_cm(); }

    /** Peak altitude in centimeters */
    get maxAltCm() { return this._m._sim_flight_max_alt_cm(); }

    /** Vertical speed in cm/s */
    get vspeedCms() { return this._m._sim_flight_vspeed_cms(); }

    /** Filtered pressure in Pascals */
    get pressure() { return this._m._sim_flight_pressure(); }

    /** Has pyro 1 fired? */
    get pyro1Fired() { return !!this._m._sim_flight_pyro1_fired(); }

    /** Has pyro 2 fired? */
    get pyro2Fired() { return !!this._m._sim_flight_pyro2_fired(); }

    /** Are pyros armed? */
    get armed() { return !!this._m._sim_flight_armed(); }

    /** Number of data samples in ring buffer */
    get samples() { return this._m._sim_flight_samples(); }

    /** Launch detection timestamp (ms) */
    get launchTime() { return this._m._sim_flight_launch_time(); }

    /** Total pyro fire count */
    get pyroFireCount() { return this._m._sim_get_pyro_fire_count(); }

    /** Last fired channel (1 or 2) */
    get lastFireChannel() { return this._m._sim_get_pyro_last_channel(); }

    /** Buzzer on/off */
    get buzzerActive() { return !!this._m._sim_get_buzzer_state(); }

    /** Accumulated NMEA telemetry length */
    get telemetryLen() { return this._m._sim_get_telemetry_len(); }

    /** Save flight data CSV (call after landing) */
    saveCsv() { this._m._sim_flight_save_csv(); }

    /** Clear telemetry buffer */
    clearTelemetry() { this._m._sim_clear_telemetry(); }

    /**
     * Snapshot of all outputs for one tick — convenient for logging.
     * @returns {object}
     */
    snapshot() {
        return {
            state: this.state,
            stateName: this.stateName,
            altitudeCm: this.altitudeCm,
            maxAltCm: this.maxAltCm,
            vspeedCms: this.vspeedCms,
            pressure: this.pressure,
            pyro1Fired: this.pyro1Fired,
            pyro2Fired: this.pyro2Fired,
            armed: this.armed,
            samples: this.samples,
            launchTime: this.launchTime,
            pyroFireCount: this.pyroFireCount,
            lastFireChannel: this.lastFireChannel,
            buzzerActive: this.buzzerActive,
        };
    }
}

/**
 * Physics engine API — drives the rocket simulation.
 *
 * The C physics engine is compiled into the same WASM module.
 * It provides a standard-atmosphere model, configurable thrust,
 * and drag with density falloff.
 *
 * Usage:
 *   const sim = await createPyroSim();
 *   const phys = sim.physics;
 *   phys.init(1524);  // 5000 ft target
 *   for (let t = 0; t < 120; t += 0.001) {
 *       phys.step(t);
 *       sim.setPressure(phys.pressurePa);
 *       sim.tick(t * 1000);
 *   }
 */
class PhysicsEngine {
    constructor(mod) {
        this._m = mod;
    }

    /** Initialize physics for a target altitude (meters) */
    init(targetAltM) { this._m._physics_wasm_init(targetAltM); }

    /** Reset all physics state */
    reset() { this._m._physics_wasm_reset(); }

    /** Step physics by 1ms. flightTimeSec = seconds since launch. */
    step(flightTimeSec) { this._m._physics_wasm_step(flightTimeSec); }

    /** Deploy drogue chute (changes drag coefficient) */
    deployDrogue() { this._m._physics_wasm_deploy_drogue(); }

    /** Deploy main chute (changes drag coefficient) */
    deployMain() { this._m._physics_wasm_deploy_main(); }

    /** Current altitude in meters */
    get altM() { return this._m._physics_wasm_alt_m(); }

    /** Current velocity in m/s (positive = up) */
    get velMs() { return this._m._physics_wasm_vel_ms(); }

    /** Current barometric pressure in Pascals */
    get pressurePa() { return this._m._physics_wasm_pressure_pa(); }

    /** Peak altitude reached in meters */
    get apogeeM() { return this._m._physics_wasm_apogee_m(); }

    /** Is the rocket on the ground? */
    get onGround() { return !!this._m._physics_wasm_on_ground(); }

    /** Is drogue deployed? */
    get drogueDeployed() { return !!this._m._physics_wasm_drogue_deployed(); }

    /** Is main chute deployed? */
    get mainDeployed() { return !!this._m._physics_wasm_main_deployed(); }

    /**
     * Convert an altitude (meters) to pressure (Pascals)
     * using the standard atmosphere model.
     */
    altToPressure(altM) { return this._m._physics_pressure_pa(altM); }

    /** Snapshot of physics state */
    snapshot() {
        return {
            altM: this.altM,
            velMs: this.velMs,
            pressurePa: this.pressurePa,
            apogeeM: this.apogeeM,
            onGround: this.onGround,
            drogueDeployed: this.drogueDeployed,
            mainDeployed: this.mainDeployed,
        };
    }
}
