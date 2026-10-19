/*
 * Flash operations under FreeRTOS. See flash_op.h for the argument.
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_op.h"
#include "core1_frame.h"
#include "flash_chip.h"
#include "flash_slot.h"
#include "rtos_tasks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/structs/timer.h"
#include "hardware/structs/watchdog.h"
#include "pico/platform.h"

/* A parked helper frees itself after this, whatever the caller does. Longer
 * than any single operation here: a 4 kB sector erase is 400 ms at most
 * (W25Q128JV_RevH_2021-03-10.pdf, page 66, MK1A; W25Q16JV_RevI_2024-12-24.pdf,
 * page 64, MK1B; MK1C's XT25F128F has no datasheet in docs/datasheets), and
 * nothing erases more at once. */
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

/* From launch to landing. flight is the flight task's, piece_busy core1's,
 * and both change under lk_lock: a piece starts only while flight holds,
 * and flight ends only while no piece runs. */
static volatile bool flight, piece_busy;
static TaskHandle_t writer;
static flash_op_slot_stats_t slot_stats;

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
    /* Before the raise to T, which under mode 0 would stop the flight task;
     * the claim below checks again under the lock. */
    if (flight) {
        slot_stats.flight_refusals++;
        refusals++;
        return -4;
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
    bool flying = flight;
    if (!flying)
        lk[other] = LK_WANTED;
    spin_unlock(lk_lock, s);
    if (flying) {
        vTaskPrioritySet(NULL, prio);
        slot_stats.flight_refusals++;
        refusals++;
        return -4;
    }
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

#define CMD_WRITE_ENABLE 0x06u
#define CMD_PAGE_PROGRAM 0x02u
#define CMD_READ_STATUS 0x05u
#define STATUS_BUSY 0x01u

/* The page program's command, address and data; filled under the lockout,
 * which one operation holds at a time. */
static uint8_t page_cmd[4u + FLASH_PAGE_SIZE];

/* Every instruction from the program command until the busy bit clears is
 * fetched from RAM: the chip answers no XIP read while it programs, though
 * flash_do_cmd() hands XIP back after each command. */
static void __no_inline_not_in_flash_func(do_program_bytes)(void *a) {
    const span_t *sp = (const span_t *)a;
    volatile uint8_t *cmdp = page_cmd; /* volatile: no memcpy() call, which lives in flash */
    cmdp[0] = CMD_PAGE_PROGRAM;
    cmdp[1] = (uint8_t)(sp->off >> 16);
    cmdp[2] = (uint8_t)(sp->off >> 8);
    cmdp[3] = (uint8_t)sp->off;
    for (uint32_t i = 0; i < sp->len; i++)
        cmdp[4u + i] = sp->data[i];
    uint8_t cmd[2] = {CMD_WRITE_ENABLE, 0};
    flash_do_cmd(cmd, cmd, 1);
    flash_do_cmd(page_cmd, page_cmd, 4u + sp->len);
    do {
        cmd[0] = CMD_READ_STATUS;
        cmd[1] = 0;
        flash_do_cmd(cmd, cmd, 2);
    } while (cmd[1] & STATUS_BUSY);
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

/* ── From launch to landing ───────────────────────────────────────── */

/* A piece that cannot suspend ends inside its slot by the datasheet; this
 * is the bound on a chip that has stopped answering, so core1 never spins
 * with its interrupts off for good. */
#define PIECE_HARD_MAX_US 20000u

/* A suspend lands in tSUS; this bounds the wait on a chip that does not. */
#define SUSPEND_HARD_MAX_US 1000u

#define CMD_ERASE_SECTOR 0x20u
#define CMD_SUSPEND 0x75u
#define CMD_RESUME 0x7Au
#define CMD_READ_STATUS2 0x35u
#define STATUS2_SUSPENDED 0x84u /* SUS (S15); on XT25F128F a program's SUS2 (S10) too */

typedef struct {
    flash_slot_kind_t kind;
    uint32_t off;
    const uint8_t *data;
    uint32_t len;
    const flash_piece_t *p;
    uint32_t settle_us;
} piece_args_t;

static uint8_t __no_inline_not_in_flash_func(chip_status)(uint8_t cmd) {
    uint8_t buf[2] = {cmd, 0};
    flash_do_cmd(buf, buf, 2);
    return buf[1];
}

static void __no_inline_not_in_flash_func(chip_cmd)(uint8_t cmd) {
    uint8_t buf[1] = {cmd};
    flash_do_cmd(buf, buf, 1);
}

static void __no_inline_not_in_flash_func(chip_start)(const piece_args_t *a) {
    volatile uint8_t *cmdp = page_cmd; /* volatile: no memcpy() call, which lives in flash */
    uint32_t n = 4u;
    cmdp[0] = a->kind == SLOT_ERASE ? CMD_ERASE_SECTOR : CMD_PAGE_PROGRAM;
    cmdp[1] = (uint8_t)(a->off >> 16);
    cmdp[2] = (uint8_t)(a->off >> 8);
    cmdp[3] = (uint8_t)a->off;
    if (a->kind == SLOT_PROGRAM) {
        for (uint32_t i = 0; i < a->len; i++)
            cmdp[4u + i] = a->data[i];
        n += a->len;
    }
    chip_cmd(CMD_WRITE_ENABLE);
    flash_do_cmd(page_cmd, page_cmd, n);
}

/* From the first command until the operation has ended or is suspended the
 * chip answers no XIP read, so every instruction is fetched from RAM; the
 * caller has core1's interrupts off. flash_do_cmd() gives XIP back after
 * each command, and the last command here finds the chip idle. */
static int __no_inline_not_in_flash_func(run_piece)(const piece_args_t *a) {
    if (a->p->resume)
        chip_cmd(CMD_RESUME);
    else
        chip_start(a);
    uint32_t t0 = timer_hw->timerawl;
    for (;;) {
        if (!(chip_status(CMD_READ_STATUS) & STATUS_BUSY))
            return PIECE_DONE;
        uint32_t now = timer_hw->timerawl;
        if (a->p->may_suspend && (int32_t)(now - a->p->suspend_at) >= 0 && (int32_t)(now - a->p->suspend_from) >= 0)
            break;
        if ((uint32_t)(now - t0) > PIECE_HARD_MAX_US)
            return -6;
    }
    chip_cmd(CMD_SUSPEND);
    uint32_t ts = timer_hw->timerawl;
    while ((uint32_t)(timer_hw->timerawl - ts) < a->settle_us) {
        __asm volatile("nop");
    }
    while (chip_status(CMD_READ_STATUS) & STATUS_BUSY) {
        if ((uint32_t)(timer_hw->timerawl - ts) > SUSPEND_HARD_MAX_US)
            return -7;
    }
    return (chip_status(CMD_READ_STATUS2) & STATUS2_SUSPENDED) ? PIECE_SUSPENDED : PIECE_DONE;
}

void flash_op_flight_begin(void) {
    uint32_t s = spin_lock_blocking(lk_lock);
    flight = true;
    spin_unlock(lk_lock, s);
}

void flash_op_flight_end(void) {
    uint32_t t0 = timer_hw->timerawl;
    for (;;) {
        uint32_t s = spin_lock_blocking(lk_lock);
        bool idle = !piece_busy || (uint32_t)(timer_hw->timerawl - t0) > PIECE_HARD_MAX_US;
        if (idle)
            flight = false;
        spin_unlock(lk_lock, s);
        if (idle)
            return;
    }
}

bool flash_op_in_flight(void) {
    return flight;
}

void flash_op_set_writer(void *task) {
    writer = (TaskHandle_t)task;
}

typedef struct {
    flash_slot_kind_t kind;
    uint32_t off;
    const uint8_t *data;
    uint32_t len;
} whole_args_t;

/* After a resume the operation runs to its end: the lockout's case, after
 * landing. */
static void __no_inline_not_in_flash_func(do_resume_finish)(void *a) {
    (void)a;
    chip_cmd(CMD_RESUME);
    while (chip_status(CMD_READ_STATUS) & STATUS_BUSY) {
    }
}

static void do_whole(void *a) {
    const whole_args_t *w = (const whole_args_t *)a;
    span_t sp = {w->off, w->data, w->len};
    if (w->kind == SLOT_ERASE)
        do_erase(&sp);
    else
        do_program_bytes(&sp);
}

static uint32_t io_now(void *ctx) {
    (void)ctx;
    return timer_hw->timerawl;
}

static uint32_t io_t0(void *ctx) {
    (void)ctx;
    return core1_frame_t0();
}

static bool io_in_flight(void *ctx) {
    (void)ctx;
    return flight;
}

static int io_whole(void *ctx, flash_slot_kind_t kind, bool resume, uint32_t addr, const uint8_t *data, uint32_t len) {
    (void)ctx;
    whole_args_t w = {kind, addr, data, len};
    return flash_op(resume ? do_resume_finish : do_whole, &w);
}

/* FLASH_SLOT_NOT_YET once the flight has ended. */
static int slot_piece(void *ctx, flash_slot_kind_t kind, uint32_t addr, const uint8_t *data, uint32_t len,
                      const flash_piece_t *p) {
    uint32_t s = spin_lock_blocking(lk_lock);
    bool go = flight;
    if (go)
        piece_busy = true;
    spin_unlock(lk_lock, s);
    if (!go)
        return FLASH_SLOT_NOT_YET;
    const flash_timing_t *t = (const flash_timing_t *)ctx;
    piece_args_t a = {kind, addr, data, len, p, t ? t->suspend_us : 0u};
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = timer_hw->timerawl;
    int rc = run_piece(&a);
    flash_op_seq++;
    uint32_t d = timer_hw->timerawl - t0;
    piece_busy = false;
    restore_interrupts(irq);
    slot_stats.pieces++;
    if (rc == PIECE_SUSPENDED)
        slot_stats.suspends++;
    if (d > slot_stats.piece_max_us)
        slot_stats.piece_max_us = d;
    return rc;
}

static int io_piece(void *ctx, flash_slot_kind_t kind, uint32_t addr, const uint8_t *data, uint32_t len,
                    const flash_piece_t *p) {
    int rc = slot_piece(ctx, kind, addr, data, len, p);
    return rc == FLASH_SLOT_NOT_YET ? io_whole(ctx, kind, p->resume, addr, data, len) : rc;
}

/*
 * [FLT-LOG-09] A piece that waits for a slot starts at the slot's start,
 * from an alarm whose interrupt is core1's: the writer's task shares core1
 * with tasks of its own priority, which its notification does not preempt,
 * and wakes up to a millisecond into the slot; BY25Q64ES's program, which
 * cannot suspend, has FLASH_SLOT_START_US to start. At the slot's start no
 * piece runs, so the flash reads.
 */
#define SLOT_ARM_MIN_US 50u
#define SLOT_WAITS 4u /* periods, each ended early by the frame's own start */

static int slot_alarm = -1;
static const flash_slot_req_t *volatile slot_req;
static volatile uint32_t slot_start;
static volatile int slot_rc;
static volatile bool slot_ran;

static void slot_isr(uint alarm) {
    (void)alarm;
    const flash_slot_req_t *r = slot_req;
    if (!r)
        return;
    flash_slot_io_t io = {r->io->ctx, io_now, io_t0, NULL, io_in_flight, slot_piece, NULL};
    flash_slot_req_t here = *r;
    here.io = &io;
    slot_rc = flash_slot_try(&here, slot_start);
    slot_req = NULL;
    slot_ran = true;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(writer, &woken);
    portYIELD_FROM_ISR(woken);
}

/* Claimed from core1, so its interrupt is core1's. */
static bool slot_alarm_ready(void) {
    if (slot_alarm < 0) {
        int a = hardware_alarm_claim_unused(false);
        if (a < 0)
            return false;
        hardware_alarm_set_callback((uint)a, slot_isr);
        slot_alarm = a;
    }
    return true;
}

/* The next slot's start far enough ahead to arm, as a time since boot. */
static uint64_t next_slot_start(uint32_t *t0) {
    uint64_t now = time_us_64();
    uint32_t at = core1_frame_t0() + LOOP_PERIOD_US;
    while ((int32_t)(at - (uint32_t)now) < (int32_t)SLOT_ARM_MIN_US)
        at += LOOP_PERIOD_US;
    *t0 = at;
    return now + (uint32_t)(at - (uint32_t)now);
}

static int io_at_slot_start(void *ctx, const flash_slot_req_t *r) {
    (void)ctx;
    slot_stats.slot_waits++;
    core1_frame_writer_done();
    if (!slot_alarm_ready()) {
        core1_frame_wait(2u * LOOP_PERIOD_MS);
        return flash_slot_try(r, core1_frame_t0());
    }
    uint32_t t0;
    uint64_t at = next_slot_start(&t0);
    slot_ran = false;
    slot_start = t0;
    slot_req = r;
    while (hardware_alarm_set_target((uint)slot_alarm, from_us_since_boot(at))) {
        at += LOOP_PERIOD_US;
        slot_start = t0 += LOOP_PERIOD_US;
    }
    for (uint32_t i = 0; i < SLOT_WAITS && !slot_ran; i++)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LOOP_PERIOD_MS));
    uint32_t irq = save_and_disable_interrupts();
    if (!slot_ran) {
        hardware_alarm_cancel((uint)slot_alarm);
        slot_req = NULL;
    }
    restore_interrupts(irq);
    return slot_ran ? slot_rc : FLASH_SLOT_NOT_YET;
}

static int slot_op(flash_slot_kind_t kind, uint32_t off, const uint8_t *data, uint32_t len) {
    if (flight && xTaskGetCurrentTaskHandle() != writer) {
        slot_stats.flight_refusals++;
        refusals++;
        return -4;
    }
    uint8_t jedec[3];
    flash_chip_jedec(jedec);
    const flash_timing_t *t = flash_timing_for(jedec);
    flash_slot_io_t io = {(void *)t, io_now, io_t0, io_at_slot_start, io_in_flight, io_piece, io_whole};
    int rc = flash_slot_run(&io, t, kind, off, data, len);
    if (rc == 0) {
        if (kind == SLOT_ERASE)
            erases++;
        else
            programs++;
    }
    return rc;
}

int flash_op_slot_erase(uint32_t offset) {
    return slot_op(SLOT_ERASE, offset, NULL, FLASH_SECTOR_SIZE);
}

int flash_op_slot_program(uint32_t offset, const uint8_t *data, uint32_t len) {
    if (!len || len > FLASH_PAGE_SIZE || (offset % FLASH_PAGE_SIZE) + len > FLASH_PAGE_SIZE)
        return -4;
    return slot_op(SLOT_PROGRAM, offset, data, len);
}

void flash_op_slot_stats(flash_op_slot_stats_t *out) {
    *out = slot_stats;
}
