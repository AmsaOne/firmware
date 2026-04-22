# Wokwi sandbox

Two browser-simulatable configurations of the forked Bruce firmware.
Neither writes to real hardware; the goal is to catch boot-time
regressions before flashing the T-Embed CC1101 Plus.

Sign in at https://wokwi.com with any GitHub account, create a new
"ESP32-S3 DevKit" project, and paste the `diagram.json` +
`wokwi.toml` from one of the subdirectories. Upload the matching
`Bruce-esp32-s3-devkitc-1.bin` from the AmsaOne/firmware
`betaRelease` (or a CI artifact from the `feature/evilportal-bridge`
branch) as the firmware image.

Wokwi simulates the full 802.11 MAC → IP → HTTP stack and exposes a
shared `Wokwi-GUEST` AP that simulated devices can STA-associate to,
which is what project B uses as the "upstream" for bridge mode.

## `bridge_off/` — flag-off parity smoke test

Purpose: prove that the fork, with `bruceConfig.evilPortalBridgeMode`
defaulted to `false`, behaves byte-for-byte like upstream Bruce when
EvilPortal is launched. If this project's serial log diverges from an
upstream run on the same simulator, something in the no-op additions
(TrafficTap scaffold, bridge-mode skeleton) regressed.

Expected serial output when EvilPortal is started with the default
configuration:

```
[PORTAL] bridge_mode=off
Evil Portal output file: default_creds.csv
```

No NAPT log line. No TrafficTap log line. No STA activity.

## `bridge_on/` — bridge-mode integration smoke test

Purpose: prove that the new code paths reach a functional state on a
simulator — STA associates, AP comes up, NAPT enables, TrafficTap
registers. This does **not** prove end-to-end victim→internet egress
because driving a second simulated client through the fake AP is
brittle in Wokwi; that case gets proven on the bench with a real
phone as the victim.

Prerequisites: the `bruce.conf` on LittleFS must contain
`"evilPortalBridgeMode": true` and `"wifi": {"Wokwi-GUEST": ""}`
(the shared open AP needs no password). Load the config via Bruce's
`/sd` browser on the simulator's web UI before triggering EvilPortal.

Expected serial output when EvilPortal is started:

```
[PORTAL] bridge_mode=on
[PORTAL] bridge_mode upstream: Wokwi-GUEST
Evil Portal output file: default_creds.csv
[PORTAL] STA -> 'Wokwi-GUEST' ...
[PORTAL] STA up: ip=10.13.37.x ch=N (was ap_ch=6) -> locking AP to STA channel
[PORTAL] NAPT enabled on STA netif
[PORTAL] TrafficTap registered (n=1)
```

If the NAPT line shows `err=...` or is missing entirely, the
precompiled lwIP stopped exporting `esp_netif_napt_enable` and we
need to revisit the pioarduino core version or vendor
`martin-ger/lwip_nat_arduino` for the ESP32-S3 target.
