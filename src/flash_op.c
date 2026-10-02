/*
 * Flash operations under FreeRTOS [DD-074].
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_op.h"
#include "rtos_tasks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/watchdog.h"
#include "pico/platform.h"

/* A parked helper frees itself after this, whatever the caller does. Longer
 * than the longest operation, one 4 kB sector erase, tSE max at -40..85 °C:
 * W25Q128JV 400 ms (RevH §9.6, MK1A), BY25Q64ES 300 ms (Rev2.9 §8.7, MK1B).
 * MK1C's XT25F128F: no datasheet in docs/datasheets. */
#define PARK_MAX_US 1500000u

/* A helper that has not parked by this is not going to: refuse the operation. */
#define PARK_WAIT_US 20000u

/* A fire sequence on MK1C takes tens of milliseconds [DD-056]. */
#define BOARD_WAIT_MS 500u

#define HELPER_STACK_WORDS 256u

enum { LK_IDLE, LK_WANTED, LK_PARKED, LK_RELEASE };

static volatile uint8_t lk[2];
static spin_lock_t *lk_lock;
static TaskHandle_t helper[2];
static StaticTask_t helper_tcb[2];
static StackType_t helper_stack[2][HELPER_STACK_WORDS];

volatile uint32_t flash_op_seq;
static volatile uint32_t lockouts, timeouts, refusals, waits, erases, programs, max_us;

__attribute__((weak)) bool board_flash_ok(void) {
    return true;
}

/* Everything from the store of LK_PARKED to the return runs from RAM with
 * interrupts off: the caller has XIP down meanwhile. __noinline because the
 * section attribute alone lets GCC inline this into helper_task(), in flash. */
void __noinline __not_in_flash_func(flash_op_park)(uint core) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t s = spin_lock_blocking(lk_lock);
    bool wanted = lk[core] == LK_WANTED;
    if (wanted)
        lk[core] = LK_PARKED;
    spin_unlock(lk_lock, s);
    if (wanted) {
        uint32_t t0 = timer_hw->timerawl;
        while (lk[core] == LK_PARKED && (uint32_t)(timer_hw->timerawl - t0) < PARK_MAX_US) {
            __asm volatile("nop");
        }
        lk[core] = LK_IDLE;
    }
    restore_interrupts(irq);
}

static void helper_task(void *arg) {
    uint core = (uint)(uintptr_t)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        flash_op_park(core);
    }
}

void flash_op_init(void) {
    lk_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
    static const char *names[2] = {"lockout0", "lockout1"};
    for (uint c = 0; c < 2; c++) {
        lk[c] = LK_IDLE;
        helper[c] = xTaskCreateStaticAffinitySet(helper_task, names[c], HELPER_STACK_WORDS, (void *)(uintptr_t)c,
                                                 PRIO_T, helper_stack[c], &helper_tcb[c], 1u << c);
    }
}

static bool wait_state(uint core, uint8_t want, uint32_t limit_us) {
    uint32_t t0 = timer_hw->timerawl;
    while (lk[core] != want) {
        if ((uint32_t)(timer_hw->timerawl - t0) >= limit_us)
            return false;
    }
    return true;
}

static void run_direct(flash_op_fn fn, void *arg) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timer_hw->timerawl;
    fn(arg);
    flash_op_seq++;
    uint32_t d = timer_hw->timerawl - t0;
    restore_interrupts(irq);
    if (d > max_us)
        max_us = d;
}

int flash_op(flash_op_fn fn, void *arg) {
    if (!rtos_running()) {
        run_direct(fn, arg);
        return 0;
    }
    if (__get_current_exception() != 0 || rtos_in_flight_task()) {
        refusals++;
        return -1;
    }
    if (!board_flash_ok()) {
        waits++;
        for (uint32_t i = 0; i < BOARD_WAIT_MS && !board_flash_ok(); i++)
            vTaskDelay(pdMS_TO_TICKS(1));
        if (!board_flash_ok()) {
            refusals++;
            return -2;
        }
    }

    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, PRIO_T);
    uint other = get_core_num() ^ 1u;

    uint32_t s = spin_lock_blocking(lk_lock);
    lk[other] = LK_WANTED;
    spin_unlock(lk_lock, s);
    xTaskNotifyGive(helper[other]);

    int rc = 0;
    if (!wait_state(other, LK_PARKED, PARK_WAIT_US)) {
        /* Withdrawn under the lock: a helper that parks after this finds
         * nothing wanted and goes back to sleep. */
        s = spin_lock_blocking(lk_lock);
        bool parked = lk[other] == LK_PARKED;
        if (!parked)
            lk[other] = LK_IDLE;
        spin_unlock(lk_lock, s);
        if (!parked) {
            timeouts++;
            rc = -3;
        }
    }
    if (rc == 0) {
        run_direct(fn, arg);
        lockouts++;
        lk[other] = LK_RELEASE;
        wait_state(other, LK_IDLE, PARK_WAIT_US);
    }
    vTaskPrioritySet(NULL, prio);
    return rc;
}

typedef struct {
    uint32_t off;
    const uint8_t *data;
    uint32_t len;
} span_t;

static void do_erase(void *a) {
    const span_t *sp = (const span_t *)a;
    flash_range_erase(sp->off, sp->len);
}

static void do_program(void *a) {
    const span_t *sp = (const span_t *)a;
    flash_range_program(sp->off, sp->data, sp->len);
}

int flash_op_erase(uint32_t offset, uint32_t len) {
    span_t sp = {offset, NULL, len};
    int rc = flash_op(do_erase, &sp);
    if (rc == 0)
        erases++;
    return rc;
}

int flash_op_program(uint32_t offset, const uint8_t *data, uint32_t len) {
    span_t sp = {offset, data, len};
    int rc = flash_op(do_program, &sp);
    if (rc == 0)
        programs++;
    return rc;
}

uint32_t flash_op_lockouts(void) {
    return lockouts;
}
uint32_t flash_op_timeouts(void) {
    return timeouts;
}
uint32_t flash_op_refusals(void) {
    return refusals;
}
uint32_t flash_op_waits(void) {
    return waits;
}
uint32_t flash_op_erases(void) {
    return erases;
}
uint32_t flash_op_programs(void) {
    return programs;
}
uint32_t flash_op_max_us(void) {
    return max_us;
}

void flash_op_crumb(unsigned n) {
    watchdog_hw->scratch[0] = 0x53540000u | (uint32_t)n;
}
