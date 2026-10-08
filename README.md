# macos-ups-glitch-filter

A small macOS daemon that stops false **"Your computer is running on UPS power"** alerts caused by a USB UPS with buggy firmware. It takes exclusive control of the UPS's USB HID interface, debounces the "on battery" signal, and republishes a clean power source to macOS. Real outages still register. One-second firmware glitches don't.

No kernel extension, no DriverKit entitlement, no SIP changes. One C file plus a LaunchDaemon.

## The problem

A USB UPS that reports itself as:

```
SmartBattery
  Product ID:   0x0300
  Vendor ID:    0x075d
  Version:      0.01
  Manufacturer: -BMS-
```

makes macOS pop up a "running on UPS power" notification every couple of minutes. The alert goes away a few seconds later. The UPS itself never clicks, beeps, or switches to battery.

`pmset -g pslog` shows macOS flipping to UPS power and back about every 2–3 minutes:

```
15:29:46  Now drawing from 'UPS Power'
          -Smart-Battery   99%; discharging; 0:00 remaining
15:29:51  Now drawing from 'AC Power'
          -Smart-Battery  100%; AC attached; not charging
```

## Diagnosis

macOS has no UPS-specific driver for this device. It enumerates as a standard USB HID Power Device. Apple's `IOUPSPlugIn` (run by `ioupsd`) reads the HID reports and publishes a power source. `powerd` watches that source and shows the alert when AC is reported lost.

Reading the raw HID values directly shows what the firmware is doing. Roughly every 2.5 minutes, **every status field resets at once for about one second**:

```
15:40:19  raw ACPresent           1 -> 0
15:40:19  raw Discharging         0 -> 1
15:40:19  raw RemainingCapacity 100 -> 99
15:40:19  raw RunTimeToEmpty 125040 -> 0
15:40:20  raw ACPresent           0 -> 1
15:40:20  raw Discharging         1 -> 0
15:40:20  raw RemainingCapacity  99 -> 100
15:40:20  raw RunTimeToEmpty      0 -> 125040
```

This isn't a power event. The BMS briefly reports a blank or default status block, likely during an internal refresh cycle. macOS polls slowly, so it holds onto the bad snapshot for about 5 seconds and shows the alert.

Other firmware quirks:
- RemainingCapacity jitters between 99 and 100 every second or two.
- RunTimeToEmpty is a fixed 125040 s (about 34.7 h), not a measured value.

## The fix

`upsfilter`:

1. **Seizes the device.** It opens the UPS with `kIOHIDOptionsTypeSeizeDevice`, so macOS's own UPS reader no longer receives reports from it.
2. **Polls the HID fields once a second:** ACPresent, Discharging, Charging, RemainingCapacity, FullChargeCapacity, RunTimeToEmpty, ShutdownImminent, BelowRemainingCapacityLimit, and BatteryPresent. It skips the event-style `ChangedStatus` collection.
3. **Debounces.** "On battery" is only accepted after it persists for `--delay` seconds (default 20). It's accepted immediately if the UPS reports ShutdownImminent. AC restore is accepted immediately.
4. **Smooths capacity.** On AC, 1% downward jitter is ignored. On battery, values are reported as-is.
5. **Republishes** a power source named **"UPS (filtered)"**. The battery menu and macOS's UPS shutdown settings (System Settings → Battery/Energy → UPS) keep working from the clean data.
6. **Logs** every raw state change and every glitch it swallowed.

## Build & try

Requires the Xcode Command Line Tools (`xcode-select --install`).

```bash
clang -O2 -Wall -o upsfilter upsfilter.c -framework IOKit -framework CoreFoundation

sudo ./upsfilter --dump       # list every HID element the UPS exposes, then exit
sudo ./upsfilter --verbose    # run in the foreground; Ctrl-C to stop
```

With the filter running you should see lines like:

```
found fields: ACPresent Discharging Charging RemainingCapacity ...
seized UPS; debounce 20s; republish on
created filtered UPS power source
ignored 1s 'on battery' blip (#1 since start)
```

and no more pop-ups.

## Install as a LaunchDaemon

```bash
sudo ./install.sh      # builds, installs to /usr/local/bin, starts at boot
tail -f /var/log/upsfilter.log
sudo ./uninstall.sh    # removes it; replug the UPS USB cable afterwards
```

To change the debounce or target a different UPS, edit the arguments in `local.upsfilter.plist` before installing.

## Options

| Option | Default | Meaning |
|---|---|---|
| `--delay N` | 20 | Seconds raw "on battery" must persist before it counts |
| `--vid ID` / `--pid ID` | 0x075d / 0x0300 | USB vendor/product ID of the UPS (hex or decimal) |
| `--no-publish` | off | Seize and log only; macOS will see no UPS at all |
| `--dump` | — | Print all HID elements and current values, then exit |
| `--verbose` | off | Also log capacity/runtime changes (noisy) |

Find your UPS's IDs with `system_profiler SPUSBDataType` or `ioreg -p IOUSB -l -w0`.

## Using it with another UPS

Any USB HID Power Device class UPS should work. Pass `--vid/--pid`, run `--dump` to confirm the standard Battery System (0x85) and Power Device (0x84) usages are present, then tune `--delay` to be longer than your UPS's glitches.

## Caveats

- **Unofficial power-source API.** Republishing uses `IOPSCreatePowerSource` / `IOPSSetPowerSourceDetails` / `IOPSReleasePowerSource`. IOKit exports these and `ioupsd` uses them, but they aren't in Apple's public headers, so a future macOS could change them. If creation fails, the daemon logs it and keeps filtering, but macOS will no longer see a UPS or shut down automatically on a low battery.
- **Stale original entry.** Depending on the macOS version, the original UPS entry may stay listed with its last-known state alongside "UPS (filtered)". Check with `pmset -g ps`.
- **Auto-shutdown depends on macOS's settings.** Shutdown on a real outage is still handled by macOS from the filtered source. Confirm your UPS shutdown thresholds are set the way you want.
- Tested on macOS 15 (Intel) with the 0x075d/0x0300 "SmartBattery" unit described above.

## License

MIT. See [LICENSE](LICENSE).
