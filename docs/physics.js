/*
 * The rocket the browser simulator flies when the WASM module is not loaded.
 * The same model as sim/physics.c, which the WASM build and the CLI simulator
 * use; keep the two in step.
 */

const G0 = 9.80665;

const PROFILES = {
    '100ft':  { target_m: 30.48,    label: '100 ft' },
    '500ft':  { target_m: 152.4,    label: '500 ft' },
    '1000ft': { target_m: 304.8,    label: '1,000 ft' },
    '5000ft': { target_m: 1524.0,   label: '5,000 ft' },
    '10000ft':{ target_m: 3048.0,   label: '10,000 ft' },
};

/* U.S. Standard Atmosphere, 1976 (NOAA-S/T 76-1562), Table 4: layer bases in
 * geopotential metres, base temperature (K), lapse rate (K/m). Base pressures
 * follow from the hydrostatic equation. */
const R_AIR = 8.31432 / 0.0289644;
const HYDROSTATIC_K = G0 / R_AIR;
const SEA_LEVEL_PA = 101325.0;
const USSA76_TOP_M = 84852.0;
const USSA76 = [
    [0, 288.15, -0.0065], [11000, 216.65, 0], [20000, 216.65, 0.0010], [32000, 228.65, 0.0028],
    [47000, 270.65, 0], [51000, 270.65, -0.0028], [71000, 214.65, -0.0020],
].map(([base_m, base_k, lapse]) => ({ base_m, base_k, lapse }));

function layerRatio(l, dh) {
    if (l.lapse === 0) return Math.exp(-HYDROSTATIC_K * dh / l.base_k);
    return Math.pow(l.base_k / (l.base_k + l.lapse * dh), HYDROSTATIC_K / l.lapse);
}

const USSA76_BASE_PA = USSA76.reduce((pa, l, i) => {
    pa.push(i === 0 ? SEA_LEVEL_PA : pa[i - 1] * layerRatio(USSA76[i - 1], l.base_m - USSA76[i - 1].base_m));
    return pa;
}, []);

function standardAtmospherePa(alt_m) {
    let i = USSA76.length - 1;
    while (i > 0 && alt_m < USSA76[i].base_m) i--;
    const l = USSA76[i];
    const h = Math.min(alt_m, USSA76_TOP_M);
    const pa = USSA76_BASE_PA[i] * layerRatio(l, h - l.base_m);
    if (alt_m <= USSA76_TOP_M) return pa;
    const top_k = l.base_k + l.lapse * (USSA76_TOP_M - l.base_m);
    return pa * Math.exp(-HYDROSTATIC_K * (alt_m - USSA76_TOP_M) / top_k);
}

function standardAtmosphereK(alt_m) {
    let i = USSA76.length - 1;
    while (i > 0 && alt_m < USSA76[i].base_m) i--;
    const l = USSA76[i];
    return l.base_k + l.lapse * (Math.min(alt_m, USSA76_TOP_M) - l.base_m);
}

function densityRatio(alt_m) {
    return (standardAtmospherePa(alt_m) / standardAtmosphereK(alt_m)) / (SEA_LEVEL_PA / USSA76[0].base_k);
}

/* Burn time whose vacuum boost and coast peak at target_m; 0 if the thrust
 * cannot lift the rocket. */
function computeBurnTime(thrust_accel, target_m) {
    const a = thrust_accel - G0;
    if (a <= 0) return 0;
    let lo = 0, hi = 200;
    for (let i = 0; i < 50; i++) {
        const t = (lo + hi) / 2;
        const h = 0.5 * a * t * t + (a * t) * (a * t) / (2 * G0);
        if (h < target_m) lo = t; else hi = t;
    }
    return (lo + hi) / 2;
}

/* Descent damping a = k·(ρ/ρ0)·|v|, k in 1/s: terminal speed at sea level g/k. */
const DROGUE_DAMPING_PER_S = 0.8;
const MAIN_DAMPING_PER_S = 4.0;
const BALLISTIC_DAMPING_PER_S = 0.05;
const STEP_S = 0.001;

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
        if (target_m > 500) this.thrust_accel = 10 * G0;
        else this.thrust_accel = 20 * G0;
        this.burn_time = computeBurnTime(this.thrust_accel, target_m);
    }

    step(flight_t_s) {
        if (this.on_ground) return;
        let a = -G0;
        if (flight_t_s < this.burn_time) a += this.thrust_accel;
        if (this.vel_ms < 0) {
            let k = BALLISTIC_DAMPING_PER_S;
            if (this.main_chute) k = MAIN_DAMPING_PER_S;
            else if (this.drogue) k = DROGUE_DAMPING_PER_S;
            a += k * densityRatio(this.alt_m) * (-this.vel_ms);
        }
        this.vel_ms += a * STEP_S;
        this.alt_m += this.vel_ms * STEP_S;
        if (this.alt_m > this.apogee_m) this.apogee_m = this.alt_m;
        if (this.alt_m <= 0) { this.alt_m = 0; this.vel_ms = 0; this.on_ground = true; }
    }

    pressurePa() {
        return standardAtmospherePa(this.alt_m);
    }
}
