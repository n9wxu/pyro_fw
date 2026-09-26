#include "fake_i2c.h"

i2c_inst_t fake_i2c_inst[2] = {{0}, {1}};
uint fake_i2c_hz[2];

#define FAKE_I2C_DEVS 4
static fake_i2c_dev_t *devs[FAKE_I2C_DEVS];
static int n_devs;

void fake_i2c_reset(void) {
    n_devs = 0;
    fake_i2c_hz[0] = fake_i2c_hz[1] = 0;
}

void fake_i2c_attach(fake_i2c_dev_t *d) {
    d->transfers = 0;
    d->hz_last = 0;
    if (n_devs < FAKE_I2C_DEVS)
        devs[n_devs++] = d;
}

uint i2c_init(i2c_inst_t *i2c, uint baudrate) {
    fake_i2c_hz[i2c->index] = baudrate;
    return baudrate;
}

uint i2c_set_baudrate(i2c_inst_t *i2c, uint baudrate) {
    fake_i2c_hz[i2c->index] = baudrate;
    return baudrate;
}

static fake_i2c_dev_t *find(i2c_inst_t *i2c, uint8_t addr) {
    if (fake_i2c_hz[i2c->index] == 0)
        return NULL; /* the peripheral is not running */
    for (int i = 0; i < n_devs; i++) {
        fake_i2c_dev_t *d = devs[i];
        if (d->bus == i2c->index && d->addr == addr && fake_func[d->sda] == GPIO_FUNC_I2C &&
            fake_func[d->scl] == GPIO_FUNC_I2C)
            return d;
    }
    return NULL;
}

int i2c_write_blocking(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, bool nostop) {
    (void)nostop;
    fake_i2c_dev_t *d = find(i2c, addr);
    if (!d)
        return PICO_ERROR_GENERIC;
    int n = d->write(d, src, len);
    if (n >= 0) {
        d->transfers++;
        d->hz_last = fake_i2c_hz[i2c->index];
    }
    return n;
}

int i2c_read_blocking(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len, bool nostop) {
    (void)nostop;
    fake_i2c_dev_t *d = find(i2c, addr);
    if (!d)
        return PICO_ERROR_GENERIC;
    int n = d->read(d, dst, len);
    if (n >= 0) {
        d->transfers++;
        d->hz_last = fake_i2c_hz[i2c->index];
    }
    return n;
}
