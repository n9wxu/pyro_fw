/*
 * Web Audio buzzer — plays tones from the WASM buzzer state.
 */
class SimBuzzer {
    constructor() {
        this.ctx = null;
        this.osc = null;
        this.playing = false;
    }

    init() {
        this.ctx = new (window.AudioContext || window.webkitAudioContext)();
    }

    update(buzzer_on) {
        if (!this.ctx) return;
        if (buzzer_on && !this.playing) {
            this.osc = this.ctx.createOscillator();
            this.osc.type = 'square';
            this.osc.frequency.value = 3000;
            this.osc.connect(this.ctx.destination);
            this.osc.start();
            this.playing = true;
        } else if (!buzzer_on && this.playing) {
            this.osc.stop();
            this.osc = null;
            this.playing = false;
        }
    }

    stop() {
        if (this.playing && this.osc) {
            this.osc.stop();
            this.osc = null;
            this.playing = false;
        }
    }
}
