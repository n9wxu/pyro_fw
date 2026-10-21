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

Click **Save**, then **Reboot**. The board merges your changes into its stored configuration — keys it does not know are ignored — and flies with them from its next start (CFG-10). Until then the Status tab says a saved change is waiting. While a flight is in progress, the file is refused and the flight keeps the configuration it started with.

### Download Flight Data

After recovery, plug in and download your CSV with one click. View it in Excel, MATLAB, or any tool you prefer. The Flight Data tab also plots the altitude profile, summarises apogee and both pyro events, and erases the log when you are done.

### Look After The Flash

The Filesystem tab shows the board's flash store: its size, what its files hold, and what is free, split into erased and ready and not yet erased. It lists every file, to download, or to delete once you confirm; the board's own files cannot be deleted. While the board's cleanup erases what deleted and replaced files left, the tab follows it until it is done (WEB-UI-07).

---

### Technical Details

- **Connection**: USB CDC-ECM network device (no drivers needed on macOS/Linux, RNDIS for Windows)
- **Address**: `http://pyro.local/` via mDNS, or `http://192.168.N.1/`, where N comes from the board's MAC so each board has a subnet of its own (`subnet` on `/api/status`)
- **Storage**: alfs on internal flash: files written only into erased pages; a cleanup erases what deleted and replaced files leave (DD-100)
- **Protocol**: Standard HTTP — works with any browser, curl, wget, or scripts
- **Tabs**: Status, Config, Flight Data, Beep Codes, Lua, Filesystem, and Update, which installs firmware and web files over the same link
- **Simultaneous access**: From launch until the flight log closes, the log alone holds the filesystem: file requests answer 423, and the status API stays live (DD-058). A change saved while the board erases its flash is held, with a note saying so, and sent once the erase is done (WEB-UI-08).
- **Boards**: every board serves the same pages.

---

*Pyro MK1B — Because your flight computer should be as easy to configure as your router.*
