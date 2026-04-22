# On-device regression checklist

Run this list on the T-Embed CC1101 Plus **after flashing the fork with
`bruceConfig.evilPortalBridgeMode` defaulted to `false`** and **before
flipping the flag to `true`**. Goal: prove the fork hasn't regressed
any existing Bruce capability when bridge mode is off.

Any failure here blocks bridge-mode testing — fix the regression
first, do not proceed to "bridge_mode=on" tests on real hardware.

Record pass/fail per run in the table at the bottom (copy the row,
fill in date, commit short-SHA, pass/fail).

## Pre-flight

- [ ] Backup firmware saved: `artifacts/pre-fork/Bruce-lilygo-t-embed-cc1101.bin`
      exists locally. If the fork bricks the board, flash this via the
      Bruce web flasher to restore stock upstream behaviour.
- [ ] SD card inserted, FAT32, with at least 1 MB free
- [ ] Battery ≥ 50% or USB powered

## Core boot

- [ ] Device boots, Bruce splash shown, no crash loop
- [ ] Serial log at 115200 shows `[PORTAL] bridge_mode=off` when
      EvilPortal is launched later (proves Phase 2b plumbing reads
      config correctly)

## WiFi (non-portal)

- [ ] `WiFi → Scan` returns at least 5 networks within 10 s
- [ ] `WiFi → Connect` joins a known open/WPA2 AP, NTP updates clock
- [ ] `WiFi → Deauth` sends frames against an isolated test SSID you
      own (confirm on a second device that it disconnects)

## Sub-GHz (CC1101, the T-Embed's raison d'être)

- [ ] `RF → Scanner` populates a frequency list
- [ ] `RF → Replay` records a known signal from a 433 MHz remote and
      replays it successfully (spectrum analyser or receiver
      confirms)
- [ ] `RF → Jammer` / `RF → Generator` start without crash (do not
      leave running — transmitting on unlicensed bands has regional
      limits)

## nRF24 (the "Plus" half of T-Embed CC1101 Plus)

- [ ] `NRF24 → Scanner` shows channel activity bars
- [ ] `NRF24 → Mousejack` / similar payloads load without crash

## IR

- [ ] `IR → Send → Power off` transmits a known IR code, a nearby TV
      or other device reacts

## Storage and UI

- [ ] `Files → SD` opens the SD card, shows top-level directories
- [ ] `Files → LittleFS` opens internal FS
- [ ] `Config → *` menus save and reload values across a reboot

## EvilPortal (with bridge_mode=off — parity with upstream)

- [ ] Launch `WiFi → Evil Portal` with default template
- [ ] Fake AP appears in a phone's WiFi list within 10 s
- [ ] Phone captive-portal popup shows the default Google template
- [ ] Submitting a test email/password writes to
      `/BruceEvilCreds/default_creds.csv` on SD
- [ ] Exit the portal cleanly with `ESC → Exit Portal`
- [ ] Serial log does NOT contain `[PORTAL] NAPT enabled` or
      `[PORTAL] TrafficTap registered` (those are bridge-on only)

## Post-checklist

If every box above is ticked, the fork is safe to flip into bridge
mode. Set `"evilPortalBridgeMode": true` in `/bruce.conf` (or via a
future UI toggle once Phase 3e lands), reboot, and re-run just the
EvilPortal row against a throwaway SSID on your own network
(10.0.0.0/24) with your phone as the victim.

## Log

| Date (YYYY-MM-DD) | Commit | Pass/Fail | Notes |
|---|---|---|---|
| _template_ | `abcdef1` | FAIL | RF scanner hung after 3 sweeps |
