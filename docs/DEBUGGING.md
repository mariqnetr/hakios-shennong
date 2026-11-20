# Debugging and the bring-up method

How the hardware here was brought up, and the tools to find out why something does not work.

## Logs and observation

* `./xl log` — the loader's log; during a run the kernel's log so far too. The kernel's `printf` goes to the loader's
  buffer, so a hang still leaves a log (the loader watches from the first core while the kernel runs on a big one).
* `./xl screenshot` — PNG of the framebuffer, also during a run from another terminal. `./xl recording` — video
  (`rec_secs=N`). `./xl sound` — WAV of what iOS played (`rec_sound=N`). `./xl camera` — frames the camera left.
* Every periodic driver line is every **5 seconds** (`fb_flip`, DVFS, thermal every `thermal_log` = 10 s, sound, Wi-Fi every
  10 s, GPU lock holders). Look at those first: they say whether a block is alive.
* `-v` boots show text on the phone's own screen. `systrace=1` traces failing syscalls. In `build/` the userland and the
  kernel have their own logs (`kernel-build.log`, `ios.log`, `gl.log`).
* `os_log` of daemons on the console: build `root.apfs` with `OSLOG_JOBS="..."` ([BUILD-AND-RUN.md](BUILD-AND-RUN.md#2-build)).
* `usbfs` pulls files off the phone during a run without stopping it.

## The bring-up method

1. **One boot-arg per step, and a log line per step.** The driver stops at step N (`sgpu=N`, `wlan=N`, `sound=N`, `cam=N`,
   `bt=N`, `adsp_step=N`). If step N hangs, the last line printed says which read/write did. Move on only when the step's
   result matches what Android does.
2. **Know what Android does.** Most steps were copied from a capture of Android running the same block (register writes,
   RPMh votes, GPR/QMI packet sequences, sensor tables) and from the Linux sources of the same SoC. When a step fails, diff
   against the capture before guessing.
3. **Read before write.** Probes that only read (`fb_probe`, `spk_probe`, `mic=1`) come before any that change state. A
   write that the SoC dislikes is an external abort and a phone reset, not a message.
4. **Add instrumentation, not retries.** The Adreno hang took a dozen rounds of *reporting* (command buffers, marks in
   the ring, performance counters, lock holders) before the fix was clear.
5. **Write down dead ends.** What an experiment ruled out is worth a comment where the code is.

## Pitfalls that cost time

| Pitfall | Do this |
|---|---|
| A flag seems ignored | The boot line is cut at about 617 characters; check the end of the `cmdline:` log line. Move the flag earlier |
| Kernel `io_map`ed a big range, then the phone hangs later | Kernel I/O mappings are permanent here. Map only the pages needed, once; never map in a loop |
| Hardware-gated clock branch "never turns on" | Do not wait on it: the hardware gates it itself |
| A rail or clock "set" but nothing happens | Vote through RPMh/cmd-db (names are 8 bytes) and check the vote is not shared with the display (camera must not take MMCX and MM0/MM1 away: the panel went dark) |
| DMA to the DSP fails | The DSP reads through its own SMMU stream; a page given by virtual address may not be visible. Use the physical page or a heap below 4 GB |
| A device is "dead" after a hang | Cold reset: `./xl reboot`. Do not trust warm state: after a hang, IFE roots, PLL3 and GMU state may be stale |
| Gestures look like an iPhone 8 | MobileGestalt cache was rebuilt at another screen class (`mg_reset=1`); see [ARCHITECTURE.md](ARCHITECTURE.md#ios-user-space) |
| Run is long and the phone is hot | Use the smallest `XL_SECS`; keep `thermal=1 thermal_batt=45 touch_boost=600` |
| Two people on one phone | One run at a time; `xl` locks, other copies' `stop`/`reboot` are refused |
| `sound_poll=1` stutters | It is a debug I²C poll; keep it off in normal runs |

## What to attach to a bug report

* the exact `XL_CMDLINE` and `XL_SECS`, the commit of this repository and of upstream, firmware version
  (`BOOT.MXF.2.1-01919.2-LANAI-1` UEFI firmware, HyperOS build);
* `./xl log` of the run (**scrub serial numbers, MAC addresses and Wi-Fi names before posting**);
* a screenshot if the screen is involved.
