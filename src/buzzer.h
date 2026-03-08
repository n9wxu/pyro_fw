#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>
#include <stdbool.h>

/* Beep codes: two digits, each 1-5 beeps */
#define BEEP_CODE(d1, d2) (((d1) << 4) | (d2))
#define BEEP_DIGIT1(code) (((code) >> 4) & 0x0F)
#define BEEP_DIGIT2(code) ((code) & 0x0F)

/* Status codes */
#define BEEP_ALL_GOOD       BEEP_CODE(1, 1)
#define BEEP_P1_OPEN        BEEP_CODE(2, 1)
#define BEEP_P1_SHORT       BEEP_CODE(2, 2)
#define BEEP_P1_FAULT       BEEP_CODE(2, 3)
#define BEEP_P1_NO_OPEN     BEEP_CODE(2, 4)
#define BEEP_P2_OPEN        BEEP_CODE(3, 1)
#define BEEP_P2_SHORT       BEEP_CODE(3, 2)
#define BEEP_P2_FAULT       BEEP_CODE(3, 3)
#define BEEP_P2_NO_OPEN     BEEP_CODE(3, 4)
#define BEEP_SENSOR_FAIL    BEEP_CODE(4, 1)
#define BEEP_FS_FAIL        BEEP_CODE(4, 2)
#define BEEP_CFG_RANGE      BEEP_CODE(4, 3)  /* pyro altitude setting out of range */
#define BEEP_CRITICAL        BEEP_CODE(5, 5)

void buzzer_init(void);
void buzzer_set_code(uint8_t code, bool repeat);
void buzzer_set_altitude(int32_t altitude);  /* apogee beep-out, repeats forever */
void buzzer_stop(void);
void buzzer_update(uint32_t now_ms);
bool buzzer_is_active(void);

/* Low-level: beep a single tone for duration_ms */
void buzzer_tone_on(void);
void buzzer_tone_off(void);

#endif
