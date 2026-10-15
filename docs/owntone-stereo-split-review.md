# OwnTone left/right stereo pair: review and test plan

Reviewed 2026-10-15. Covers your OwnTone fork (`hungngit2/owntone-server`), the `ytb-owntone-dashboard` repo, and every option for making two AirPlay speakers (the R1 units at 10.0.1.1 and 10.0.1.2) act as one left/right pair without drifting apart.

## Summary

- **Option 3, the fork, already exists and is running.** The OwnTone 29.3 on the Wyse (10.0.0.100) exposes a per-output `channels` field and `misc.airplay_sync_interval_ms = 100`, and upstream has neither. The drift you hear is happening with this build.
- **The sender side of the fork looks correct.** Left and right each get their own AirPlay session, and both sessions are timed from the same player clock. The most likely cause is that the R1 receivers don't follow OwnTone's clock. That is still a hypothesis. Step 0 (section 3) proves or rules it out in about 2–3 hours.
- **If the receivers are the cause, no server-side change can fix it.** That includes more fork tweaks and PipeWire. The fix is a receiver that follows the sender's clock: a Linux board running shairport-sync (option E), or an Android receiver app that does the same (option D). I could not verify any Android app that does. The receiver doesn't need AirPlay 2 for this; an AirPlay 1 receiver that handles timing properly works with the fork.
- **Two things restart or interrupt audio during playback today:**
  - The dashboard can force-stop the receiver apps on both speakers while a song plays (section 2, finding 1).
  - The fork pauses and resumes channel-split outputs every 5 minutes by default (`playback_resync_minutes = 5`).

  Turn both off before measuring, or the results won't mean anything.

## 1. What is running now (checked 2026-10-15, about 09:40)

| Item | Value |
| --- | --- |
| OwnTone | 29.3, custom build, library name "My Music on Wyse-5070" |
| `GET /api/outputs` | Includes a `channels` field ("unset" / "left" / "right"). This is a fork addition, not upstream. |
| `misc.airplay_sync_interval_ms` | 100. Upstream behaviour is equivalent to 1000. |
| Outputs at time of check | Only "Computer" (ALSA), selected. No AirPlay outputs listed. |
| R1 speakers | adb port 5555 on 10.0.1.1 and 10.0.1.2 was not reachable, so the speakers were probably off or offline |
| Speaker protocol | AirPlay 1 (RAOP), according to the fork's own design note `docs/superpowers/specs/2026-08-01-airplay-sync-interval-design.md` |
| Receiver apps on R1 | `com.autodlna` and `com.example.echoservice` are the apps the dashboard restarts. Which one actually serves AirPlay is not confirmed (see 4.D step 2). |

What the fork changes for channel splitting (local checkout `/Users/hung.nguyen/Data/workspace/owntone-server`):

- A per-output channel transform in the ALSA, Pulse, FIFO, RAOP and AirPlay backends (`channel_transform()` in `src/misc.c`).
- AirPlay and RAOP master sessions keyed by channel mode, so left and right never share an encoded stream (`master_session_make()` in `src/outputs/airplay.c` and `src/outputs/raop.c`).
- A configurable sync-packet interval (`misc.airplay_sync_interval_ms`).
- Drift workarounds in `src/player.c`:
  - `idle_resync_minutes` (default 15) and `playback_resync_minutes` (default 5) reconnect channel-split outputs on a timer.
  - Commit 9c5de368 re-aligns outputs during playback with a pause and resume.
  - Commit 26a29f6f skips that resync for pipes and streams that have no length.

## 2. Dashboard review (`ytb-owntone-dashboard`)

All line numbers refer to `public/backend.php` at commit f1a441f.

1. **It restarts the receiver apps during playback (high impact).**
   - When it runs:
     - The daemon's `advance_queue_if_finished()` calls `check_and_manage_airplay_outputs()` on every 2-second tick while the player state is `play` (lines 2335–2337).
     - `play_url_body()` calls it before every play (line 1990).
   - What it does: if either speaker is missing, or `channels` isn't exactly left/right, it runs `restart_r1_speaker_services()` (lines 1712–1720). That force-stops `com.autodlna` and `com.example.echoservice` on both speakers over adb, then calls `sleep(2)`, all while holding the playback lock.
   - Why it matters: the only throttle is a 30-second cooldown. A speaker that drops out triggers a restart of both speakers, including the healthy one, about every 30 seconds for the rest of the song. Force-stopping the receiver app resets its sync, so a drift test run while this is active can't be trusted.
   - Fix: never restart receivers while the state is `play`. Restart only the speaker that is missing, and only when idle or on an explicit action. For the test period, put the whole function behind a config flag.
2. **It falls back to selecting non-AirPlay outputs (medium impact).**
   - When no AirPlay outputs are found, `$airplayOutputs = $outputs` (lines 1735–1737 and 1811–1813) makes "Computer" (ALSA) a candidate and selects it. That matches what the server shows right now: Computer is selected.
   - Fix: if no AirPlay outputs are found, do nothing.
3. **Left and right can swap (medium impact).**
   - If the output names don't contain "left" or "right", the code assigns L/R by array order from `/api/outputs` (lines 1754–1757 and 1830–1833). That order isn't guaranteed after the speakers are rediscovered.
   - Fix: match each speaker by a configured name or IP, for example `SPEAKER_LEFT_NAME` and `SPEAKER_RIGHT_NAME`.
4. **The cooldown file may not be shared (low impact).**
   - The cooldown lives in `sys_get_temp_dir()`. `docs/queue-daemon.service` has no `PrivateTmp`, but php-fpm units on Debian and Ubuntu commonly set `PrivateTmp=true`. Find the unit with `systemctl list-units 'php*-fpm*'`, then check it with, for example, `systemctl show php8.2-fpm -p PrivateTmp`.
   - If the two differ, php-fpm and the daemon each keep their own 30-second cooldown.
   - Fix: keep the cooldown file under `/mnt/appsrv/ytb-owntone/data/`.
5. **The detect-and-assign block is duplicated (cleanup).** Lines 1730–1766 repeat as lines 1806–1843. Extract a helper so a fix lands in one place.
6. **The README may describe the wrong host.** It describes a 4-core ARM board with about 971 MB RAM, but OwnTone now reports "Wyse-5070". If the deployment moved, update the README so future debugging starts on the right box.

Side finding in the fork: with the web-auth username empty, `GET /api/settings` returns `webinterface.auth_password` in plain text to anyone on the LAN. Consider leaving secrets out of that response.

## 3. Step 0: find out where the drift comes from (do this first)

The goal is to decide between a **sender problem**, which you fix in the fork, and a **receiver problem**, which you fix by changing the receiver. The tests below answer that.

### 3.1 Prepare (about 10 minutes)

1. Stop the dashboard's interference: `sudo systemctl stop queue-daemon`. During the test, play from OwnTone's own web UI, not the dashboard.
2. Turn off the fork's resync workarounds in `owntone.conf` under `general`, then restart OwnTone. A value of 0 or less disables them.
   ```
   idle_resync_minutes = 0
   playback_resync_minutes = 0
   ```
3. Write down each speaker's current `offset_ms`, `channels` and `type`:
   ```bash
   curl -s http://10.0.0.100:3689/api/outputs | jq '.outputs[] | {name, type, channels, offset_ms}'
   ```
4. Set `misc.airplay_sync_interval_ms` back to 1000 (upstream behaviour) for the first runs.
5. For run A only, add a known +20 ms to one speaker's `offset_ms` (for example, the right speaker's current value plus 20). Both speakers play the same click in run A, so without this gap the two clicks overlap in the recording whenever they are less than a few ms apart, and that is exactly the range you need to see. Remove the extra 20 ms after run A.

### 3.2 Test tracks

Both commands were checked on 2026-10-15. Each produces a 60-minute FLAC file of a few MB with a 1 ms click once per second.

L/R track. The left channel clicks at 2 kHz and the right channel at 1 kHz:

```bash
ffmpeg -f lavfi -i "aevalsrc='if(lt(mod(t,1),0.001),sin(2*PI*2000*t),0)|if(lt(mod(t,1),0.001),sin(2*PI*1000*t),0)':s=44100:d=3600" -c:a flac clicks_LR_60min.flac
```

Mono baseline track, the same 2 kHz click on both channels:

```bash
ffmpeg -f lavfi -i "aevalsrc='if(lt(mod(t,1),0.001),sin(2*PI*2000*t),0)':s=44100:d=3600" -ac 2 -c:a flac clicks_mono_60min.flac
```

Copy both files into an OwnTone library directory and rescan.

### 3.3 How to measure

- Put a phone exactly halfway between the speakers and measure the distances with a tape. Sound travels about 34 cm per ms, so a 3 cm error is about 0.1 ms.
- Record 20 seconds at 0, 15, 30 and 60 minutes.
- In Audacity, switch to spectrogram view and measure the time between the two click onsets.
  - Run A: subtract the 20 ms you added. What's left is the real offset.
  - Run B: 2 kHz is the left speaker and 1 kHz the right, so you also know which one leads.
- How to read the results:
  - **The offset stays the same:** you only need a static correction. Set `offset_ms` and you're done.
  - **The offset grows steadily:** the two clocks are drifting.
  - **The offset jumps in steps, with an audible glitch:** a receiver is skipping or repeating audio to catch up.
- Rough perception guide: about 1 ms is enough to pull a centred voice toward the earlier speaker. Offsets in the tens of milliseconds start to sound like an echo.

### 3.4 Runs

| Run | Setup | What it tells you |
| --- | --- | --- |
| A | Both speakers `channels = unset`, mono track, +20 ms on one speaker | Whether plain OwnTone multiroom keeps the two R1s in sync. No split involved. |
| B | `channels = left/right`, L/R track | Whether the split path adds drift |
| C (only if B drifts and A doesn't) | Same as B with `airplay_sync_interval_ms = 100` | Whether faster sync packets help |

- **A drifts:** the problem is the receivers, or possibly the network. No change on the sending side will fix it, so go to option D or E.
- **A is stable but B drifts:** there's a bug in the fork's split path. Go to option A.
- **A and B are both stable:** the drift you heard came from the resync workarounds or the dashboard restarts. Keep both disabled and set `offset_ms`.

### 3.5 Check whether the receivers follow OwnTone's clock at all

- In AirPlay 1, the receiver sends timing requests to the sender. OwnTone answers them in `timing_svc_cb()` (`src/outputs/raop.c`, around line 3187), which expects packets starting `0x80 0xd2`. A receiver that never sends them is running on its own clock and will drift whatever the server does.
- On the Wyse, during playback, find OwnTone's RAOP timing port (it's random) in the OwnTone log, or with `ss -ulpn | grep owntone`. Then watch for requests from each speaker:
  ```bash
  sudo tcpdump -ni any -c 40 'udp and (src host 10.0.1.1 or src host 10.0.1.2)'
  ```
- The result: if one speaker sends small packets to the timing port every few seconds, it is at least trying to sync. If you see none at all, that's strong evidence the receiver is the problem.

### 3.6 Optional A/B against a known-good receiver

Run shairport-sync on a laptop or Linux box as a third AirPlay receiver; Homebrew and most Linux distros package it. Pair it with one R1 (shairport-sync on the left, R1 on the right) and repeat run B. If R1 + shairport-sync drifts the same way as R1 + R1, the R1 is the clock that's moving.

## 4. Options reviewed

### A. The fork (already built): keep it, but stop adding sync workarounds until Step 0 points at the sender

What looks right:
- `master_session_make()` includes `channels == ams->channels` in its match, so left and right get separate encoders and RTP sessions.
- The transform runs on a private copy (`ams->chanbuf`), so the shared buffer isn't modified for other sessions.
- Each session calls `timestamp_set(ams, obuf->pts)` with the same player pts, so both sessions sit on one timeline.
- `outputs_quality_subscribe()` (`src/outputs.c:663`) is reference-counted, so two sessions at the same quality share one transcode safely.

Concerns:
- `airplay_sync_interval_ms` only changes how often the sender sends sync packets. If a receiver ignores those packets or applies them badly, the setting can't help. The fork's own design note calls it an unproven hypothesis.
- The `player.c` workarounds interrupt audio by design. With the defaults, that happens every 5 minutes during playback. They hide drift rather than fix it, so keep them off for Step 0 and remove them if Step 0 points at the receivers.
- Maintenance cost: the fork differs from upstream by about 2,000 lines across 34 source files, including the YouTube and web-auth work. Keep the channel split as its own commit series so it can be rebased or offered upstream.

What to try, only if Step 0 says the sender is at fault:
- Turn on debug logging for the RAOP and AirPlay modules.
- Confirm that both sessions' sync packets map the same NTP time to the same sample position.
- Test the case where one speaker joins mid-track (a new master session created mid-stream) against both starting together.

### B. PipeWire combine-stream: skip

- It would replace the fork's in-process split with a weaker one: two RAOP sinks, each with its own timing. PipeWire issue 3419 already reports an unstable stereo image with two HomePods.
- It uses the same RAOP protocol to the same R1 receivers, so any receiver-side drift stays.
- It also needs extra plumbing:
  - OwnTone's Pulse output pointed at the pipewire-pulse socket (`audio { type = "pulseaudio" server = "unix:/run/pipewire/pulse/native" }`).
  - PipeWire running as a system service.
  - The dashboard's reconcile logic turned off, because it would re-select the AirPlay outputs.
- The only reason to try it is as a cheap second opinion if Step 0 is inconclusive.

### C. Native stereo pairing on the speakers: probably not available

I found no source showing the R1 supports stereo pairing. Check whether the current receiver app (probably `com.autodlna`; confirm as in 4.D step 2) has a left/right channel setting. If it does, set OwnTone `channels = unset` and let the app do the split. That still only helps if the receiver keeps sync, so Step 0 run A still decides.

### D. Install a different receiver app (APK) on the R1

To stop drift, the app has to **follow the sender's clock and correct for drift**. AirPlay 2 isn't required:
- With AirPlay 1, the receiver syncs to OwnTone's RAOP timing service, which the fork already serves.
- With AirPlay 2, the receiver syncs over PTP.

Either way, the app also has to run on the R1's Android version.

| Candidate | What I found | Verdict |
| --- | --- | --- |
| [jqssun/android-airplay-server](https://github.com/jqssun/android-airplay-server) (Play, F-Droid, GitHub) | Built on UxPlay and requires Android 7.0+. The UxPlay README says "AirPlay2 multi-room audio streaming is not supported: use shairport-sync for that." UxPlay is aimed at single-device mirroring and audio. | Not a fit for synced multi-speaker audio |
| DroidPlay | An Android TV / Google TV receiver in early development. A summary says its RAOP (AirPlay 1) audio path has a jitter buffer and clock-drift compensation, which is the right feature. I couldn't read the repository itself (GitHub rate limit) and found no published APK; it builds from source with compileSdk 36, so recent Android only. | Best on paper, unverified, likely needs a newer Android than the R1 has |
| Commercial receivers (AirReceiver, AirPin, BoostVision AirMirror) | I found no documentation of drift correction or multiroom sync | Test only, don't count on it |
| shairport-sync on the R1 itself | A classic AirPlay 1 build follows the sender clock and needs no nqptp. The real blocker is audio output: shairport-sync expects ALSA, PulseAudio or similar, and Android apps don't get those without root and a custom native build. I found no Android or Termux port. (An AirPlay 2 build would also need nqptp running as root with UDP ports 319 and 320.) | Experimental; root and a native build required |

How to evaluate any APK in about an hour:

1. Check what the R1 can run:
   ```bash
   adb connect 10.0.1.1:5555
   adb -s 10.0.1.1:5555 shell getprop ro.build.version.release
   adb -s 10.0.1.1:5555 shell getprop ro.build.version.sdk
   adb -s 10.0.1.1:5555 shell getprop ro.product.cpu.abilist
   ```
   An Android version below 7.0 rules out jqssun/android-airplay-server.
2. Confirm which installed app actually serves AirPlay before disabling anything. `com.autodlna` is only a guess based on the dashboard's restart helper.
   - On the Mac, list the AirPlay receivers and get each one's port:
     ```bash
     dns-sd -B _raop._tcp local.
     dns-sd -L "<name from the list>" _raop._tcp local.
     ```
   - On the R1, find which uid listens on that port:
     ```bash
     adb -s 10.0.1.1:5555 shell cat /proc/net/tcp /proc/net/tcp6
     ```
     The port is in hex in the local-address column. A listening socket has state `0A`, and the uid is its own column.
   - Match the uid to a package:
     ```bash
     adb -s 10.0.1.1:5555 shell dumpsys package com.autodlna | grep userId=
     ```
3. Install the APK with `adb install -r <app>.apk` and make it start on boot. Disable the old receiver so two receivers don't advertise the same speaker:
   ```bash
   adb -s 10.0.1.1:5555 shell pm disable-user --user 0 <old receiver package>
   ```
   To undo it later:
   ```bash
   adb -s 10.0.1.1:5555 shell pm enable <old receiver package>
   ```
4. In OwnTone, check that the new receiver appears and note its `type` (AirPlay 1 or 2).
5. Repeat Step 0 run A. It passes if the offset stays stable over 60 minutes.

Verdict: I found no APK that I could verify follows the sender clock well enough for a stereo pair. Try one only if the R1 meets its Android version requirement. Otherwise go to option E.

### E. A Linux receiver running shairport-sync: the most reliable option

- **Hardware:** a Raspberry Pi Zero 2 W or better per speaker. shairport-sync recommends Pi 2 / Zero 2 W class hardware or better for AirPlay 2. Add a DAC or amp HAT driving the speaker. The R1 could only serve as the speaker if it has an analog input, which I couldn't verify.
- **Software:** shairport-sync. A classic AirPlay 1 build already follows OwnTone's clock and corrects drift. An AirPlay 2 build with nqptp is optional. One board uses `playback_mode = "both left";` and the other `playback_mode = "both right";` in the `general` section.
- **OwnTone:** set both outputs to `channels = unset`, so the receiver does the split. Upstream OwnTone would also work, so the fork's channel code isn't needed.
- **Verify:** run Step 0 runs A and B on the new receivers.
- **Cost:** two boards plus DACs or amps, and about a weekend of setup.

### F. Snapcast (Snapdroid on the R1): not recommended for now

Snapcast is designed around sync, and an Android client (Snapdroid) exists. However, I found no per-client left/right channel selection. A forum workaround mutes one side on each client, which isn't a real pair. Revisit only if option E is impossible.

## 5. Decision tree

```
Step 0 run A (no split) drifts?
├─ yes → receiver problem. Server-side fixes won't help.
│        ├─ R1 meets a candidate app's Android requirement → option D (APK test)
│        └─ otherwise, or D fails → option E (Linux + shairport-sync)
└─ no  → run B (split) drifts?
         ├─ yes → fork split bug → option A diagnostics
         └─ no  → set offset_ms, keep the resync workarounds and dashboard restarts off. Done.
```

Before any of this, apply dashboard findings 1–3, or at least disable the reconcile function.

## 6. Prompts for each step

Each prompt is self-contained, so you can paste it into a new session.

### P1. Step 0 diagnostics

```
Context: OwnTone 29.3 (my fork hungngit2/owntone-server, local checkout /Users/hung.nguyen/Data/workspace/owntone-server) runs on my Wyse at 10.0.0.100 (ssh -i ~/.ssh/id_hungnguyen root@10.0.0.100). It plays to two AirPlay 1 speakers (R1 units, 10.0.1.1 and 10.0.1.2) set up as a manual left/right pair using the fork's per-output "channels" field. After a while the two speakers drift apart. Read /Users/hung.nguyen/Data/Claude/owntone-stereo-split-review.md, sections 1 and 3, first.

Goal: decide whether the drift comes from the sender (OwnTone) or the receivers (R1), following section 3 of that file.

Do:
1. Preparation (3.1): stop queue-daemon, set idle_resync_minutes = 0 and playback_resync_minutes = 0 in owntone.conf [general], set misc.airplay_sync_interval_ms = 1000, add +20 ms offset_ms to one speaker for run A, and restart OwnTone. Before changing anything, record the current values and the /api/outputs state so I can restore them. Show me each command before running anything on the server.
2. Generate both test tracks (3.2), copy them to the OwnTone library directory and trigger a rescan.
3. Run check 3.5: find OwnTone's RAOP timing port and capture 60 s of tcpdump while playing. Tell me whether each speaker sends timing requests.
4. Guide me through runs A and B (3.4). I record with my phone and tell you the measured offsets at 0/15/30/60 min. You interpret them using the rules in 3.3 and 3.4.
5. Give me a verdict (sender / receiver / static offset only) with the evidence. Then restore the settings I want to keep.

Don't change OwnTone source code in this task.
```

### P2. Dashboard fixes

```
Repo: /Users/hung.nguyen/Data/workspace/ytb-owntone-dashboard (PHP + vanilla JS dashboard for OwnTone). Read /Users/hung.nguyen/Data/Claude/owntone-stereo-split-review.md section 2 first. Verify each finding against the current code before changing anything.

Fix, in public/backend.php:
1. check_and_manage_airplay_outputs() must never restart the R1 receiver apps while OwnTone's player state is "play". Restart only the speaker that is actually missing, and only when idle. Add a config constant to disable the whole reconcile (default: enabled, current behaviour otherwise).
2. If no AirPlay outputs are found, do nothing. Remove the fallback that treats every output (e.g. "Computer" ALSA) as a candidate.
3. Assign left and right by configured speaker name or IP (new constants), not by /api/outputs array order.
4. Move the cooldown file out of sys_get_temp_dir() to the data directory next to QUEUE_STATE_FILE.
5. Remove the duplicated detect-and-assign block by extracting a helper.
Add tests in tests/backend_test.php for the pure parts (speaker matching, decide-whether-to-act). Run php tests/backend_test.php and node tests/app_test.js. Keep the existing code style. Create a branch, but don't commit or push until I review.
```

### P3. Evaluate a different receiver app on the R1

```
I have two R1 smart speakers (Android, adb over network at 10.0.1.1:5555 and 10.0.1.2:5555) used as AirPlay receivers for OwnTone. They drift apart over long playback. Read /Users/hung.nguyen/Data/Claude/owntone-stereo-split-review.md, sections 3 and 4.D, first.

Goal: find out whether any Android receiver app follows the sender's clock and corrects drift well enough for a left/right pair on these speakers. AirPlay 1 (RAOP timing) or AirPlay 2 (PTP) are both fine.

Do:
1. Read the Android version, SDK level and CPU ABI list on both speakers, and check whether adb root works. Show the commands first; don't change anything yet.
2. Confirm which installed package actually serves AirPlay (dns-sd port -> /proc/net/tcp uid -> package, as in section 4.D step 2). Don't assume it is com.autodlna.
3. Research current Android AirPlay receivers (open source and commercial) for documented clock sync / drift compensation and minimum Android version. Cite sources and mark anything unverified. Known so far: jqssun/android-airplay-server is UxPlay-based, and UxPlay says multiroom isn't supported. DroidPlay claims drift compensation on RAOP but is early-stage and needs a recent Android.
4. If one candidate fits this Android version, give me the exact install and test steps: adb install, start on boot, `pm disable-user --user 0` for the old receiver with the `pm enable` rollback, and how to confirm it in OwnTone /api/outputs.
5. Then I run Step 0 run A from the review file and report the offsets. Tell me whether it passes.
If nothing fits, say so plainly and recommend option E.
```

### P4. Linux receivers running shairport-sync

```
I want to replace two AirPlay receivers with Linux boards running shairport-sync so they keep sync as a left/right stereo pair behind OwnTone (Wyse, 10.0.0.100). Read /Users/hung.nguyen/Data/Claude/owntone-stereo-split-review.md, section 4.E, first.

Do:
1. Recommend hardware per speaker (board, DAC or amp HAT) for my speakers: <describe speakers / whether they are passive or have line-in>. Check current shairport-sync AirPlay 2 hardware guidance and cite it.
2. Recommend a classic (AirPlay 1) or AirPlay 2 + nqptp build of shairport-sync for this setup, and explain why. Then write the full build and setup: OS image, shairport-sync build, systemd units, config with name = "Left" / "Right" and playback_mode = "both left" / "both right", output device, and volume handling.
3. Configure OwnTone: both outputs channels = unset, and check their type in /api/outputs. Note anything needed for the IoT VLAN (mDNS, firewall, and UDP 319/320 if using nqptp).
4. Give me the verification plan using Step 0 runs A and B from the review file. The pass criterion is that the offset stays stable within 1 ms over 60 minutes.
Don't buy or flash anything for me. Give me the list and the steps.
```

### P5. Fork follow-up (only if Step 0 says the sender is at fault)

```
My OwnTone fork (/Users/hung.nguyen/Data/workspace/owntone-server, origin hungngit2/owntone-server) splits stereo across two AirPlay 1 speakers using per-output "channels". A measured test (Step 0 in /Users/hung.nguyen/Data/Claude/owntone-stereo-split-review.md) showed that plain multiroom stays in sync but the left/right split drifts: <paste the measured offsets>.

Goal: find and fix the drift in the split path.

Do:
1. Review the channel-split changes against upstream/master in src/outputs/raop.c, src/outputs/airplay.c, src/misc.c and src/player.c. Focus on master_session_make keying, timestamp_set/packets_sync_send per session, and what happens when a second master session is created mid-stream.
2. Turn on debug logging for the RAOP and AirPlay modules and capture one playback. Show me whether both sessions' sync packets map the same NTP time to the same sample position over time.
3. Propose the smallest fix, explain why it addresses the measured symptom, and wait for my approval before coding.
4. After the fix, remove the player.c resync workarounds (idle/playback resync, pause-resume re-align) if they're no longer needed, and repeat Step 0 run B.
Keep the change isolated in its own commits on a feature branch. Don't push without asking.
```

## Sources

- [UxPlay README](https://github.com/FDH2/UxPlay) ("AirPlay2 multi-room audio streaming is not supported: use shairport-sync for that")
- [jqssun/android-airplay-server](https://github.com/jqssun/android-airplay-server)
- [shairport-sync sample config, `playback_mode`](https://raw.githubusercontent.com/mikebrady/shairport-sync/master/scripts/shairport-sync.conf)
- [shairport-sync AirPlay 2 notes (nqptp, hardware)](https://git.sudo.is/mirrors/shairport-sync/src/branch/master/AIRPLAY2.md)
- [PipeWire combine-stream module](https://docs.pipewire.org/page_module_combine_stream.html)
- [PipeWire issue 3419, unstable stereo image](https://gitlab.freedesktop.org/pipewire/pipewire/-/issues/3419)
- [Mopidy forum: split channel stream with Snapcast](https://discourse.mopidy.com/t/split-channel-stream/2020)
