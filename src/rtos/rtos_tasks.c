/*
 * The tasks. See rtos_tasks.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "rtos_tasks.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "flash_op.h"
#if PYRO_HAS_SD
#include "hr_log.h"
#endif
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include <string.h>

_Static_assert(PRIO_T == configMAX_PRIORITIES - 1, "the lockout must be at the top priority (plan 2, 4.2 rule 1)");
_Static_assert(PRIO_P + 1 == PRIO_T, "nothing between P and T (plan 2, 4.2 rule 2)");

/* The task bodies, elsewhere. */
void flight_task(void *arg);  /* main_hardware.c */
void net_task(void *arg);     /* net_task.c */
void storage_task(void *arg); /* storage_task.c */

static TaskHandle_t h_flight, h_net, h_storage;
static StaticTask_t tcb_flight, tcb_net, tcb_storage;
static StackType_t stack_flight[FLIGHT_STACK_WORDS];
static StackType_t stack_net[NET_STACK_WORDS];
static StackType_t stack_storage[STORAGE_STACK_WORDS];

static volatile bool running;

bool rtos_running(void) {
    return running;
}

bool rtos_in_flight_task(void) {
    return running && xTaskGetCurrentTaskHandle() == h_flight;
}

/* ── The mailbox ──────────────────────────────────────────────────── */

enum { CALL_NONE, CALL_PENDING, CALL_RUNNING, CALL_DONE };

static SemaphoreHandle_t call_mutex;
static StaticSemaphore_t call_mutex_buf;
static volatile uint8_t call_state;
static flight_call_fn call_fn;
static void *call_arg;
static spin_lock_t *call_lock;

bool flight_call(flight_call_fn fn, void *arg, uint32_t timeout_ms) {
    if (!running || rtos_in_flight_task()) {
        fn(arg);
        return true;
    }
    TickType_t t0 = xTaskGetTickCount();
    if (xSemaphoreTake(call_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;
    call_fn = fn;
    call_arg = arg;
    __dmb();
    call_state = CALL_PENDING;
    while (call_state != CALL_DONE && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(timeout_ms))
        vTaskDelay(pdMS_TO_TICKS(1));
    /* Withdrawn only while still pending: once the flight task has taken it,
     * it runs, and this waits for it. */
    uint32_t s = spin_lock_blocking(call_lock);
    bool ran = call_state != CALL_PENDING;
    if (!ran)
        call_state = CALL_NONE;
    spin_unlock(call_lock, s);
    while (ran && call_state != CALL_DONE)
        vTaskDelay(pdMS_TO_TICKS(1));
    call_state = CALL_NONE;
    xSemaphoreGive(call_mutex);
    return ran;
}

void flight_call_service(void) {
    uint32_t s = spin_lock_blocking(call_lock);
    bool take = call_state == CALL_PENDING;
    if (take)
        call_state = CALL_RUNNING;
    spin_unlock(call_lock, s);
    if (!take)
        return;
    call_fn(call_arg);
    __dmb();
    call_state = CALL_DONE;
}

/* ── Hand-offs ────────────────────────────────────────────────────── */

void rtos_notify_storage(void) {
    if (h_storage)
        xTaskNotifyGive(h_storage);
}

/* ── The storage task's check-in ──────────────────────────────────── */

/* A pass of the storage task takes at most one sector erase, 400 ms, and
 * whatever it waited for the filesystem. */
#define STORAGE_STALL_MS 5000u

static volatile uint32_t storage_seen_ms;
static volatile bool storage_seen;
static uint32_t stalls;
static bool stalled;

void storage_checkin(uint32_t now_ms) {
    storage_seen_ms = now_ms;
    storage_seen = true;
}

bool storage_alive(uint32_t now_ms) {
    bool alive = !storage_seen || (uint32_t)(now_ms - storage_seen_ms) < STORAGE_STALL_MS;
    if (!alive && !stalled)
        stalls++;
    stalled = !alive;
    return alive;
}

uint32_t storage_stalls(void) {
    return stalls;
}

/* ── Creation ─────────────────────────────────────────────────────── */

#if PYRO_HAS_LUA
void lua_task_create(void); /* lua_core1.c: created now, runs once launched */
#endif

void rtos_start(struct flight_context_t *ctx) {
    call_mutex = xSemaphoreCreateMutexStatic(&call_mutex_buf);
    call_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
    flash_op_init();
    h_flight = xTaskCreateStaticAffinitySet(flight_task, "flight", FLIGHT_STACK_WORDS, ctx, PRIO_P, stack_flight,
                                            &tcb_flight, CORE0_ONLY);
    h_net =
        xTaskCreateStaticAffinitySet(net_task, "net", NET_STACK_WORDS, NULL, PRIO_P, stack_net, &tcb_net, CORE1_ONLY);
    h_storage = xTaskCreateStaticAffinitySet(storage_task, "storage", STORAGE_STACK_WORDS, NULL, PRIO_P, stack_storage,
                                             &tcb_storage, CORE1_ONLY);
#if PYRO_HAS_LUA
    lua_task_create();
#endif
#if PYRO_HAS_SD
    hr_log_create_tasks();
#endif
    running = true;
    vTaskStartScheduler();
    for (;;) {
        /* Unreachable: the scheduler never returns. */
    }
}

/* ── Stack reports ────────────────────────────────────────────────── */

int rtos_task_stats(rtos_task_stat_t *out, int max) {
    TaskHandle_t hs[] = {h_flight, h_net, h_storage};
    const char *names[] = {"flight", "net", "storage"};
    int n = 0;
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]) && n < max; i++) {
        if (!hs[i])
            continue;
        out[n].name = names[i];
        out[n].free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(hs[i]) * sizeof(StackType_t);
        n++;
    }
    return n;
}

/* ── The kernel's hooks ───────────────────────────────────────────── */

/* The flight task's stage stamp, numbered from 200 so /api/status's
 * prev_stage reads it apart, and read by firmware older than this too: an
 * image that dies and rolls back still says why. scratch[1] carries the
 * detail. The watchdog resets the board. */
#define RTOS_STAMP(code) (0x53540000u | (200u + (code)))

/* The first four characters of a name, packed, for scratch[1]. */
static uint32_t pack4(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && s && s[i]; i++)
        v |= (uint32_t)(uint8_t)s[i] << (8 * i);
    return v;
}

void rtos_assert_failed(const char *file, int line) {
    watchdog_hw->scratch[0] = RTOS_STAMP(1);
    watchdog_hw->scratch[1] =
        ((uint32_t)line & 0xffffu) | (pack4(strrchr(file, '/') ? strrchr(file, '/') + 1 : file) << 16);
    save_and_disable_interrupts();
    for (;;) {
    }
}

/* PICO_PANIC_FUNCTION (CMake): the SDK's panic() prints through stdio, which
 * under FreeRTOS takes a mutex -- a wait prove_core0.py finds on the flight
 * task's path. This prints nothing: it stamps, stops, and the watchdog resets
 * the board. */
void __attribute__((noreturn)) pyro_panic(const char *fmt, ...) {
    watchdog_hw->scratch[0] = RTOS_STAMP(3);
    watchdog_hw->scratch[1] = pack4(fmt);
    save_and_disable_interrupts();
    for (;;) {
    }
}

/* A HardFault stamps 204 and the faulting PC, from whichever stack the fault
 * pushed its frame on. Overrides the SDK's weak handler. */
void __attribute__((used, noreturn)) rtos_hardfault_c(const uint32_t *frame) {
    watchdog_hw->scratch[0] = RTOS_STAMP(4);
    watchdog_hw->scratch[1] = frame[6];
    for (;;) {
    }
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile("movs r0, #4\n"
                   "mov r1, lr\n"
                   "tst r0, r1\n"
                   "beq 1f\n"
                   "mrs r0, psp\n"
                   "b 2f\n"
                   "1: mrs r0, msp\n"
                   "2: ldr r1, =rtos_hardfault_c\n"
                   "bx r1\n");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    watchdog_hw->scratch[0] = RTOS_STAMP(2);
    watchdog_hw->scratch[1] = pack4(name);
    save_and_disable_interrupts();
    for (;;) {
    }
}
