/*
 * Physics engine for Pyro MK1B browser simulation.
 * Drives the WASM flight computer black box.
 * Separate from pyro code — this is the external simulation process.
 */

const G = 9.81;
const GROUND_PA = 101325.0;
const PAD_DWELL_MS = 2000;

const PROFILES = {
    '100ft':  { target_m: 30.48,    label: '100 ft' },
    '500ft':  { target_m: 152.4,    label: '500 ft' },
    '1000ft': { target_m: 304.8,    label: '1,000 ft' },
    '5000ft': { target_m: 1524.0,   label: '5,000 ft' },
    '10000ft':{ target_m: 3048.0,   label: '10,000 ft' },
};

function standardAtmospherePa(alt_m) {
    if (alt_m < 11000)
        return GROUND_PA * Math.pow(1 - 0.0065 * alt_m / 288.15, 5.2561);
    const p11 = GROUND_PA * Math.pow(1 - 0.0065 * 11000 / 288.15, 5.2561);
    if (alt_m < 47000)
        return p11 * Math.exp(-9.81 * (alt_m - 11000) / (287.05 * 216.65));
    const p47 = p11 * Math.exp(-9.81 * 36000 / (287.05 * 216.65));
    return p47 * Math.exp(-9.81 * (alt_m - 47000) / (287.05 * 270.65));
}

function computeBurnTime(thrust_accel, target_m) {
    let lo = 0, hi = 200;
    for (let i = 0; i < 50; i++) {
        const t = (lo + hi) / 2;
        const a = thrust_accel - G;
        const h = 0.5 * a * t * t + (a * t) * (a * t) / (2 * G);
        if (h < target_m) lo = t; else hi = t;
    }
    return (lo + hi) / 2;
}

class PhysicsEngine {
    constructor() {
        this.reset();
    }

    reset() {
        this.alt_m = 0;
        this.vel_ms = 0;
        this.drogue = false;
        this.main_chute = false;
        this.on_ground = false;
        this.thrust_accel = 0;
        this.burn_time = 0;
        this.apogee_m = 0;
    }

    setProfile(target_m) {
        this.reset();
        if (target_m > 500) this.thrust_accel = 10 * G;
        else this.thrust_accel = 20 * G;
        this.burn_time = computeBurnTime(this.thrust_accel, target_m);
    }

    step(flight_t_s) {
        if (this.on_ground) return;
        let a = -G;
        if (flight_t_s < this.burn_time) a += this.thrust_accel;
        if (this.vel_ms < 0) {
            let drag = 0.05;
            if (this.main_chute) drag = 4.0;
            else if (this.drogue) drag = 0.8;
            a += drag * Math.exp(-this.alt_m / 8500) * (-this.vel_ms);
        }
        this.vel_ms += a * 0.001;
        this.alt_m += this.vel_ms * 0.001;
        if (this.alt_m > this.apogee_m) this.apogee_m = this.alt_m;
        if (this.alt_m <= 0) { this.alt_m = 0; this.vel_ms = 0; this.on_ground = true; }
    }

    pressurePa() {
        return standardAtmospherePa(this.alt_m);
    }
}
