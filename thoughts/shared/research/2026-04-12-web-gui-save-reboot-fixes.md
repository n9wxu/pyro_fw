# Web GUI Save/Reboot Issues - Root Cause and Fixes

**Date:** 2026-04-12  
**Issue:** Save button returns error, reboot button doesn't show visible effect

## Root Cause Analysis

### Save Error (HTTP 409 Conflict)

**Problem:**
- Pressure sensor fails to initialize intermittently (BMP280 detection fails)
- When sensor fails, `pp_feed()` is never called
- Calibration requires 10 samples to complete (`PP_CAL_SAMPLES`)
- Device gets stuck in `BOOT_CALIBRATE` state waiting for samples
- HTTP server rejects config save with 409 Conflict because `state != PAD_IDLE`

**Evidence:**
```
UART log lines 437-445:
!BMP280 no response at 0x76
!BMP280 no response at 0x77
!BMP280 detect FAIL
!PRES trying MS5607 (SDA=10)
!PRES init FAIL: no sensor found
```

### Reboot Button Issue

**Problem:**
- Reboot code IS working correctly (`watchdog_reboot()` called)
- Device keeps getting stuck in boot states after reboot
- User doesn't see visible feedback because device never reaches PAD_IDLE

## Implemented Fixes

### 1. Added Calibration Timeout (src/flight_states.c)

**Change:** Added 10-second timeout to `BOOT_CALIBRATE` state
```c
static state_event_t detect_boot_calibrate(flight_context_t *ctx, uint32_t now) {
    if (pp_cal_done())
        return SEVT_CAL_DONE;

    /* Timeout if calibration takes too long (sensor failure) */
    if (now - ctx->boot_timer >= 10000) {
        extern void hal_telemetry_send(const char *sentence);
        hal_telemetry_send("!CAL TIMEOUT - sensor failed, forcing PAD_IDLE\r\n");
        return SEVT_CAL_DONE; /* Force transition to PAD_IDLE */
    }

    return SEVT_NONE;
}
```

**Benefit:**
- Device no longer hangs forever if sensor fails
- After 10s timeout, transitions to PAD_IDLE
- Config saves now succeed even with sensor failure
- UART log shows clear diagnostic message

### 2. Improved HTTP Server Error Handling (src/http_server.c)

**Changes:**
a) Added debug logging for all config operations:
```c
DBG("POST /api/config cl=%lu", (unsigned long)content_length);
DBG("POST /api/config REJECT state=%u (need PAD_IDLE=3)", (unsigned)state);
DBG("POST /api/config OK (applied)");
DBG("POST /api/reboot");
```

b) Improved error response to show actual state:
```c
snprintf(err_msg, sizeof(err_msg),
         "HTTP/1.1 409 Conflict\r\n" CORS_HDR "Connection: close\r\n"
         "Content-Type: application/json\r\n\r\n"
         "{\"error\":\"Device not ready (state=%s)\","
         "\"state\":\"%s\",\"reboot_required\":true}",
         state_names[state], state_names[state]);
```

**Benefits:**
- Clear diagnostic messages in UART log
- User sees actual state name in error message
- Easier to debug issues remotely

### 3. Enhanced Web GUI Error Display (www/app.js)

**Change:** Parse and display server error messages
```javascript
.then(function(r) {
  if (r.ok) {
    // ... success handling ...
    return null;
  } else {
    return r.json().catch(function() { return {error: 'Save failed'}; });
  }
})
.then(function(err) {
  if (err) {
    msg.style.color = 'red';
    msg.textContent = ' ' + (err.error || 'Error saving');
  }
})
```

**Benefits:**
- User sees actual error message: "Device not ready (state=BOOT_CALIBRATE)"
- Clear indication of what's preventing the save
- Better user experience with actionable error messages

## Testing Recommendations

1. **Test normal operation:** Verify config save works when sensor initializes correctly
2. **Test sensor failure:** Disconnect sensor, verify timeout occurs and PAD_IDLE reached
3. **Test reboot:** Verify reboot works and device returns to PAD_IDLE
4. **Check UART logs:** Confirm debug messages appear for all operations

## Related Issues

- Sensor initialization reliability (may need retry logic)
- I2C bus reset after sensor failure
- Better user feedback during boot sequence