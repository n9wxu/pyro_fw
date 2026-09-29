/*
 * FreeRTOS SMP on the RP2040 [DD-073]. The design is
 * docs/log_storage_plan2_freertos.md (plan 2), and rtos_tasks.h names the
 * tasks and their priorities.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ── Scheduling: plan 2, section 4 ────────────────────────────────────
 *
 * One priority level runs at a time (configRUN_MULTIPLE_PRIORITIES 0, the
 * user's direction): the flight task and the tasks on core1 share P and run
 * together; a lockout at T stops them all. Nothing sits between P and T. */
#define configNUMBER_OF_CORES 2
#define configRUN_MULTIPLE_PRIORITIES 0
#define configUSE_CORE_AFFINITY 1
#define configUSE_TASK_PREEMPTION_DISABLE 0
#define configUSE_PASSIVE_IDLE_HOOK 0
#define configMAX_PRIORITIES 3 /* idle 0, P 1, T 2 */

/* The tick interrupts core1, where the tasks that share by time slicing
 * live. Core0 holds the flight task alone and is woken by its own alarm. */
#define configTICK_CORE 1

#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configUSE_TICKLESS_IDLE 0
#define configTICK_RATE_HZ ((TickType_t)1000)
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD 1

/* ── Memory: static only ──────────────────────────────────────────── */
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configKERNEL_PROVIDED_STATIC_MEMORY 1
#define configSTACK_DEPTH_TYPE uint32_t
#define configMINIMAL_STACK_SIZE ((configSTACK_DEPTH_TYPE)256)
#define configMESSAGE_BUFFER_LENGTH_TYPE size_t

/* ── Features ─────────────────────────────────────────────────────── */
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_COUNTING_SEMAPHORES 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configQUEUE_REGISTRY_SIZE 0
#define configUSE_QUEUE_SETS 0
#define configUSE_NEWLIB_REENTRANT 0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_CO_ROUTINES 0
#define configUSE_APPLICATION_TASK_TAG 0
#define configENABLE_BACKWARD_COMPATIBILITY 0

/* The SDK's pico_sync and pico_time block at the kernel level from a task.
 * The interop needs the timer daemon, which lives at P on core1. */
#define configSUPPORT_PICO_SYNC_INTEROP 1
#define configSUPPORT_PICO_TIME_INTEROP 1
#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY 1
#define configTIMER_QUEUE_LENGTH 10
#define configTIMER_TASK_STACK_DEPTH 512
#define configTIMER_SERVICE_TASK_CORE_AFFINITY (1u << 1)

/* ── Checks ───────────────────────────────────────────────────────── */
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0
#define configUSE_TRACE_FACILITY 1
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configGENERATE_RUN_TIME_STATS 0
#define configRECORD_STACK_HIGH_ADDRESS 1

/* An assertion stamps where it failed into the watchdog scratch registers
 * and stops; the watchdog resets the board and /api/status reports it. */
#ifndef __ASSEMBLER__
void rtos_assert_failed(const char *file, int line);
#define configASSERT(x)                                                                                                \
    do {                                                                                                               \
        if (!(x))                                                                                                      \
            rtos_assert_failed(__FILE__, __LINE__);                                                                    \
    } while (0)
#endif

/* ── API ──────────────────────────────────────────────────────────── */
#define INCLUDE_vTaskPrioritySet 1
#define INCLUDE_uxTaskPriorityGet 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle 1
#define INCLUDE_eTaskGetState 1
#define INCLUDE_xTimerPendFunctionCall 1
#define INCLUDE_xTaskAbortDelay 1
#define INCLUDE_xTaskGetHandle 0
#define INCLUDE_xTaskResumeFromISR 1
#define INCLUDE_xQueueGetMutexHolder 1

#endif
