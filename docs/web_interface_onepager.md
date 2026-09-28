# Pyro MK1B Web Interface

## Configure your flight computer from any browser. No drivers. No special software.

---

**Plug in USB. Open your browser. You're ready to fly.**

The Pyro MK1B presents itself as a USB network device. Your laptop automatically connects. Navigate to `http://pyro.local/` and you have full control.

---

### Live Pre-Flight Dashboard

See everything at a glance before you walk to the pad:

- ✅ **Pyro 1**: OK — continuity, with its ADC reading
- ✅ **Pyro 2**: OK — continuity, with its ADC reading
- 📊 **Altitude**: 0 m (calibrated)
- ⚙️ **State**: PAD_IDLE — Ready for launch

It also shows whether a USB host is attached, and a **Test mode** switch: on USB the board detects no launch and stays quiet until you turn it on (USB-01, USB-08). Once loaded, the dashboard stays live in flight.

### Edit Configuration Instantly

Change deployment settings right in your browser. No file transfers, no ejecting, no corruption risk.

```
Pyro 1: Delay 0s after apogee (drogue)
Pyro 2: AGL 300m (main)
Units: meters
```

Click **Save**. Done. The board takes changes only while it sits on the pad, merges them into its running configuration — keys it does not know are ignored — and applies them at once. A pin released to Lua takes effect at the next reboot.

### Download Flight Data

After recovery, plug in and download your CSV with one click. View it in Excel, MATLAB, or any tool you prefer. The Flight Data tab also plots the altitude profile, summarises apogee and both pyro events, and erases the log when you are done.

---

### Web interface compared with a USB drive

| | USB Drive (traditional) | Web Interface (Pyro MK1B) |
|---|---|---|
| **Setup** | Plug in, find drive in Finder | Plug in, open browser |
| **Edit config** | Open file in text editor, save, eject | Edit in browser, click Save |
| **See device status** | ❌ Not possible | ✅ Live dashboard |
| **Verify pyro continuity** | ❌ Need separate tool | ✅ Shown on dashboard |
| **Risk of corruption** | ⚠️ Must eject safely | ✅ No corruption possible |
| **Special software** | None | None |
| **Works on** | macOS, Windows, Linux | macOS, Windows, Linux |

---

### Technical Details

- **Connection**: USB CDC-ECM network device (no drivers needed on macOS/Linux, RNDIS for Windows)
- **Address**: `http://pyro.local/` via mDNS or the board's own DNS, or `http://192.168.N.1/`, where N comes from the board's MAC so each board has a subnet of its own (`subnet` on `/api/status`)
- **Storage**: littlefs on internal flash with built-in wear leveling
- **Firmware overhead**: ~48KB code, ~20KB RAM additional
- **Protocol**: Standard HTTP — works with any browser, curl, wget, or scripts
- **Tabs**: Status, Config, Flight Data, Beep Codes, Lua, and Update, which installs firmware and web files over the same link
- **Simultaneous access**: Firmware reads config while you browse. No conflicts. From launch until the flight log closes, the log alone holds the filesystem: file requests answer 423, and the status API stays live (DD-058).
- **Boards**: MK1A and MK1C serve the same pages.

---

*Pyro MK1B — Because your flight computer should be as easy to configure as your router.*
