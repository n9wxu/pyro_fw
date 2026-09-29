#ifndef FAKE_TASK_H
#define FAKE_TASK_H
#include "FreeRTOS.h"
static inline TaskHandle_t xTaskCreateStaticAffinitySet(void (*fn)(void *), const char *name, uint32_t depth, void *arg,
                                                        UBaseType_t prio, StackType_t *stack, StaticTask_t *tcb,
                                                        UBaseType_t cores) {
    (void)fn, (void)name, (void)depth, (void)arg, (void)prio, (void)stack, (void)tcb, (void)cores;
    return (TaskHandle_t)1;
}
static inline void xTaskNotifyGive(TaskHandle_t t) {
    (void)t;
}
static inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    (void)clear, (void)wait;
    return 0;
}
static inline void vTaskDelayUntil(TickType_t *prev, TickType_t inc) {
    (void)prev, (void)inc;
}
static inline TickType_t xTaskGetTickCount(void) {
    return 0;
}
#endif
