/* Just enough of FreeRTOS for code whose task loops the host tests step by
 * hand (test_hr_log.c): the types, and nothing that schedules. */
#ifndef FAKE_FREERTOS_H
#define FAKE_FREERTOS_H
#include <stdint.h>
typedef void *TaskHandle_t;
typedef struct {
    int unused;
} StaticTask_t;
typedef uint32_t StackType_t;
typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY 0xFFFFFFFFu
#endif
