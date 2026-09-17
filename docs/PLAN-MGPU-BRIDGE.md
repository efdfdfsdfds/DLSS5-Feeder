# Plan: DLSS5-Feeder ↔ MGPU Bridge (Neural-coprocessor) compatibility

## Review and implementation status (2026-09-17, branch `mgpu`)

The plan below was reviewed against MGPU Bridge's source (0.2.3, `de8bf97`) and ReShade 6.8.0's
before anything was written. The mechanism it rests on held up. These points did not, and the
implementation follows the corrections, not the text further down:

| Plan said | What is true | Consequence |
|---|---|---|
| The add-on is `MGPU_Bridge*.addon64` | It is `nvngx.dll_mgpu_bridge.addon64`, and the name is load-bearing on MGPU's side | Detection matches `*mgpu_bridge*.addon64` everywhere |
| `mgpu.ini` lives in `mgpu\` | It is read from beside the add-on; only `nvngx_dlssnr.dll` goes in `mgpu\` (or any immediate subfolder) | Layout checks, README, Verify |
| Read `mgpu.ini` with `GetPrivateProfileIntA` | MGPU has its own parser: no sections, first line-start `Key=` wins, `;`/`#` comment lines. Absent keys default to Calib 2, **MVec 0, MvecFromEval 0**, Depth 0, DepthInverted 1 | `MgpuIniFind` in `feed_mgpu.h` mirrors it; a missing `mgpu.ini` is reported as "takes no vectors" |
| Exclude MGPU's runtime by comparing adapter LUIDs | Its window class is `MGPU_Bridge_Wnd_<module>`, which needs no bound runtime to compare against | `RuntimeSlot::bridge` and `PresentChain::foreign` key on the class |
| Interception depends on import-table order | MGPU also scans every module's writable data for cached NGX pointers (R102) and rescans for 30 s, so a feeder that resolved first is still reached | No ordering work needed |
| Host: `CopyResource(bb, Output)` plus an R32F view on `h.tex[FEED_DEPTH]` | The game overwrites Depth as soon as `fence_out` fires, which is before the pump queue's tap runs | The helper copies Depth into a private `SIMULTANEOUS_ACCESS` texture on the evaluate's own list and binds THAT |
| `h.queue` may wait on the pump fence | It must never: an uncomposited window can hold the pump queue inside DXGI, and the game waits on `h.queue` | Only `pump_queue->Wait(h.fence, …)`; the other direction does not exist |
| Installer appends the shader path to host64's `ReShade.ini` | The helper does it itself before ReShade loads, so manual installs get it too | No installer code for it |
| `UPSTREAM-MGPU-BRIDGE.md` at the repo root | Notes live in `docs/` now | `docs/UPSTREAM-MGPU-BRIDGE.md` |

Done: A1, A2, A3, B1 (D3D11-family clients; a GL or Vulkan game leaves the helper on the default
adapter), B2, B3, B4, C. `FEED_IPC_VERSION` is unchanged (one new ack bit, one new optional
argument). Not done: the window-mode latency matrix in B3 (needs a game and, to mean anything, a
second GPU) — `MgpuLatchClear` defaults to 1 and the window mode is whatever `host_window` says.

Verified here (one RTX 5090): all three binaries build with no warnings; the no-game rig in
`docs/DEPLOY-DEV.md` §9c passes with MGPU 0.2.3 (300/300, one present per evaluate, debug layer
clean with a proven-live layer), with `--mgpu-frames` alone, and with no MGPU at all (ordinary mode
unchanged); `Verify-DLSS5Feeder.ps1` against good, bad and two-consumer fixtures. NOT verified:
anything in a real game, the 32-bit add-on's new code at run time, the installer's MGPU branch at
run time (both scripts parse), and everything that needs MGPU to arm.

---

## Context

[maohgad-web/Neural-coprocessor](https://github.com/maohgad-web/Neural-coprocessor) ("MGPU Bridge", MIT, 0.2.2 tagged, 0.2.3 on main) is a ReShade add-on that runs DLSS-NR on a **second GPU** and presents it in its own window. It credits DLSS5-Feeder (ACKNOWLEDGEMENTS.md:74). It is **D3D12-only**, and it gets motion vectors in **one way only**: its "calibrator" swaps `GetProcAddress` in every module's import table, catches `NVSDK_NGX_D3D12_CreateFeature` / `NVSDK_NGX_D3D12_EvaluateFeature` (exact names), and copies the `MotionVectors` resource at the evaluate. In games with no DLSS it runs with no vectors at all.

The feeder already makes exactly that call with validated optical-flow vectors. Checked in `build\dlss5-feed.addon64`: the NGX static lib resolves the non-`_C` names through the add-on's own import table, so MGPU should intercept the feeder unchanged. The goal is to make that pairing work deliberately and safely:
- **In-process** for 64-bit D3D12 games.
- **Through the host64 helper** for 32-bit games (D3D11, D3D10, D3D9/dgVoodoo, OpenGL, Vulkan/DXVK).

**User decisions:**
- Scope: D3D12 pairing plus the host64 route.
- 64-bit D3D11/Vulkan/GL are **deferred**; they get a warning only.
- DLAA evaluate and write-back stay as they are.
- Upstream issue drafted **locally only**.
- Installer: no MGPU download; it just must not break an MGPU layout.

**Constraints:**
- The working tree on `fix/recent-log-bugs-2026-09-10` has uncommitted user work (PR #110, #92/#100/#114, DFC AbortCommands, README). Build on top of it, never revert it, and commit nothing unless asked.
- This machine (RTX 5090 + AMD iGPU) cannot run MGPU end-to-end: it refuses without a second neural-capable GPU. The calibrator still installs, so interception is observable here.

## Verified facts the design rests on

**What MGPU needs:**
- **Colour:** the back buffer at the game runtime's `reshade_finish_effects`. Only runtimes on the adapter LUID of the selected game swapchain, and only D3D12 (MGPU dllmain.cpp:1794, 1639-1643).
- **Depth:** effect texture **named** `MGPU_DepthOutTex`, written by `mgpu_depth_tap.fx` from the `: DEPTH` semantic, raw. `Depth=1` holds arming until it is bound.
- **Vectors:** there is no named-texture or MOTION lane. Evaluate interception needs:
  - `Calib`≠0, `MVec=3`, `MvecFromEval` 1, or 2 (2 = auto after 300 frames with no copies).
  - A barrier from `PIXEL|NON_PIXEL` shader resource to `COPY_SOURCE` on the caller's command list.
  - The same vector handle stable for 240 frames.
- **Handle cap:** it latches **at most 4** feature-1/13 handles and **never releases** them (MGPU calibrator.cpp:209-252). Every feeder re-create burns a slot for good.
- **Layout:** `nvngx_dlssnr.dll` must sit in `<exe>\mgpu\`; a copy beside the exe gives a red INSTALL PROBLEM. `mgpu.ini` is `[MGPU]`. `ReShade2.ini` has `PresetPath=.\gpu1.ini` for its GPU-1 runtime.

**What ReShade does:**
- It fires `finish_effects` only when techniques are loaded (runtime.cpp on_present gate).
- It skips the whole present hook for `DXGI_PRESENT_TEST`, and for `DO_NOT_WAIT` right after `WAS_STILL_DRAWING` (dxgi_swapchain.cpp on_present).
- Generic Depth rebinds `DEPTH` to 0 on effect reload when it has no depth-stencil.

**Feeder facts:**
- **64-bit D3D12:** the evaluate happens on the game queue inside `reshade_render_technique` (dlss5-feed.cpp:6217). Vectors are parked `PIXEL|NON_PIXEL` (6381), which is what MGPU expects. The write-back (6448) lands before MGPU's capture.
- **Swapchain bug:** `OnInitSwapchain` stores the *last* swapchain (846-850). MGPU's GPU-1 swapchain would then drive `PresentColorSpace()` → `BridgePqWanted` (3622).
- **Host64:**
  - It presents `R8G8B8A8` at window size with `DO_NOT_WAIT` (host:1683); the back buffer holds the banner or undefined content.
  - It has no ReShade add-on registration, and its `ReShade.ini` has `EffectSearchPaths=.\` and no .fx.
  - Its device is on DXGI's default adapter (host:1889), created before the pipe exists.

## Phase P0: baseline rig (no code)

1. Download MGPU 0.2.2 release zip into `deploy/mgpu-bridge/` (add a `deploy/SOURCES.md` line). If the repo is ever cloned under `external/`, add a `.gitignore` line first.
2. Build a scratch rig like DEPLOY-DEV §9: host exe, ReShade x64 `dxgi.dll`, `MGPU_Bridge.addon64`, `mgpu\{nvngx_dlssnr.dll,mgpu.ini}`, `ReShade2.ini`, `gpu1.ini`, the tap .fx, and `nvngx_dlss.dll`. Run `--test --hide`.
3. Expect 300/300 evaluates, MGPU's `[MGPU][T2] REFUSING … single-adapter` line, and calibrator lines showing it latched `dlss5-feed-host64.exe`'s CreateFeature(1) and MV_Scale.
4. Repeat in-process in a 64-bit D3D12 game (Armored Core VI is deployed; move its renodx-dlss5 aside and restore it afterwards).

## Phase A: in-process D3D12 pairing (64-bit add-on)

### A1. Coexistence fixes (`src/dlss5-feed.cpp`)
- **Swapchain by runtime:**
  - Replace `g_swapchain` with a small `{swapchain*, HWND}` array maintained in `OnInitSwapchain`/`OnDestroySwapchain`.
  - `PresentColorSpace()` returns the one whose `get_hwnd()` matches `g.runtime->get_hwnd()`, falling back to the last one.
  - Log the window class per swapchain.
  - This also fixes the Smooth Motion proxy case. Callers: 3622, 3758, 5139, 5425, 5807.
- **Bridge-runtime exclusion:**
  - Add `luid` and `bridge` to `RuntimeSlot` (7973-8014).
  - A runtime whose D3D12 device sits on a different adapter LUID from the bound game runtime, while MGPU is loaded, is never adopted (8162-8163, 8229, 8304) and does not reset `create_grace` (8190).

### A2. Detection and guards: new `src/feed_mgpu.h`
Header-only, modelled on `src/feed_opti.h`, with no NGX headers so `dlss5-feed32.cpp` can include it too.
- **Detection:**
  - `MgpuScan(dir)`: `MGPU_Bridge*.addon64` (warn if more than one), plus `mgpu\mgpu.ini`, `mgpu\nvngx_dlssnr.dll`, `ReShade2.ini` with `PresetPath=.\gpu1.ini`, `gpu1.ini`, and whether `mgpu_depth_tap.fx` is reachable.
  - `MgpuLoaded()`: loaded module whose `NAME` export string is "MGPU Bridge".
  - `MgpuIni`: `GetPrivateProfileIntA` for Calib, CalibRung, MVec, MvecFromEval, Depth, DepthInverted, SRUpscale, NoActivate.
- **Policy: never write mgpu.ini; warn only.**
- **In `dlss5-feed.cpp`:**
  - Call `DetectMgpu()` next to `DetectOptiScaler()` (attach order ~8700).
  - Add globals `g_mgpu_*` and an overlay block beside OptiScaler's (8455-8476).
  - Add MGPU's file name to the fault attribution for `g_ngx_poisoned` (2604-2606).
- **Warnings** (log + `Warn`, once each):
  - `Calib=0`, `MVec`≠3, or `MvecFromEval=0`: no vectors will reach it.
  - Another neural consumer (Chicken/RenoDX/OptiScaler/Toolkit) is also present: neural rendering runs twice.
  - Non-D3D12 API: MGPU does nothing here but still patches import tables; remove it.
  - `nvngx_dlssnr.dll` beside the exe.
  - `DepthInverted` differs from the feeder's *effective* value (`g_cfg.depth_inverted >= 0 ? … : g.depth_reversed`, 3585/3722).
  - `RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN`, scale or offset defines set: MGPU's raw tap ignores them, so depth and vectors will be misaligned.
  - `enabled=0`, `mode`≠2, or `DLSS5_Feed` not rendering: no evaluate means no vectors.
  - The game has its own DLSS/Streamline: extend the warning at 2588.
  - Smooth Motion runtime present: it shifts MGPU's `ReShade2.ini`.
- **Handle cap:**
  - `WarmupRebuildDue()` (2654) returns false when MGPU is the only consumer.
  - Count feature creates; after the 4th, warn that MGPU stops latching until restart.

### A3. Scripts and docs
- **`tools/Verify-DLSS5Feeder.ps1`:**
  - MGPU-only counts as a valid consumer (branch before the Fail at 1325-1330).
  - `nvngx_dlssnr.dll` is required in `<consumerDir>\mgpu\`; beside the exe it is a Warn, not the Fail at 1393-1398. Pass the subfolder explicitly, since `Find-FileIn` is not recursive.
  - Check `ReShade2.ini`/`gpu1.ini`/tap .fx; MGPU plus another consumer is a Fail; D3D12-only note for 64-bit.
  - Read MGPU's `[MGPU]` lines out of ReShade.log: T2 refusal, INSTALL PROBLEM.
- **`tools/Install-DLSS5Feeder.ps1`** (no MGPU download):
  - When an MGPU layout is detected, skip copying `nvngx_dlssnr.dll` beside the exe (2620-2632) and do not install a second consumer; keep `nvngx_dlss.dll`.
  - For 32-bit games, move a misplaced MGPU set into `host64\` (2376-2380 pattern).
  - Append the shaders path to an existing host64 `ReShade.ini` `EffectSearchPaths` (2778-2785).
  - Keep PS 5.1, CRLF and BOM as the files have them.
- **README:** "Alternative: MGPU Bridge (second GPU)" after the OptiScaler section (~555), plus TOC, a line in "Before you install" §2, troubleshooting, and Limitations. State plainly that the pairing is unverified end-to-end.
- **DEPLOY-DEV.md:** new §9c for the MGPU rig (after §9b, 484-550).

## Phase B: host64 route for 32-bit games

### B1. Game adapter on the host command line
- **Client:** `HostWorkerConnect` (dlss5-feed32.cpp:2256-2259) appends `--adapter-luid=HHHHHHHH:LLLLLLLL`. LUID sources:
  - D3D11/dgVoodoo: `IDXGIDevice::GetAdapter`.
  - D3D10: `g.d10.luid` (4611).
  - GL: `GL_DEVICE_LUID_EXT` where supported.
  - Vulkan: only if a physical device is reachable; otherwise omit the flag.
- **Host:**
  - Parse it in `main` (3522-3534).
  - In `InitDisguise`, create the factory before the device, call `EnumAdapterByLuid`, and pass the adapter to `create_device` (1889). Fall back to null with a log line.
  - Fix the "(DXGI's default adapter)" text (2032).
- **No `FEED_IPC_VERSION` bump:** an old host ignores unknown args. This also fixes #100.

### B2. Host detection, exclusion, ReShade add-on registration
- `DetectMgpuBridge()` in host `main` after `DetectOptiScaler()` (3548), using `feed_mgpu.h`.
- **MGPU mode is on only when MGPU is the sole consumer in host64.** Otherwise it logs a WARNING and keeps today's behaviour. OptiScaler plus MGPU is always refused.
- **When MGPU mode is on:**
  - `warm_done=true` (2784, 3087).
  - MGPU branch in `LogNeuralConsumerOutcome` (1067-1103).
  - MGPU name in `NoteNgxFault` (1113-1116).
- **Add-on registration:**
  - In MGPU mode, after `create_device` (so after the built-in add-ons) and before `CreateSwapChainForHwnd` (1941), call `reshade::register_addon(GetModuleHandleW(nullptr))`.
  - Add `/I..\external\reshade\include` to `host\build-host.bat`.
  - Events: `init_effect_runtime`, `reshade_reloaded_effects`, `reshade_open_overlay`, `present`, `reshade_finish_effects`.
  - Counters logged every 1800 frames: evaluates / Presents / present events / finish_effects / WAS_STILL_DRAWING. Warn when finish_effects stays at 0, which means no tap .fx on host64's effect path.

### B3. Frame mode: host presents the DLAA output for MGPU to capture
- **Swapchain:**
  - On each `'B'` build, `ResizeBuffers` to `out_w×out_h` in `FeedFmtTypedColor(out_fmt)` (feed_fmt.h:44). Accept only the RGBA8, BGRA8, RGB10A2 and RGBA16F families; otherwise frame mode is off for that build.
  - Set the colour space from `b.hdr` and the format.
  - Keep window size separate from buffer size (`g_bb_w/h`): `WM_SIZE` (1246-1250) and `'W'` (3103-3128) resize only the window, and `RefitHostOverlay` uses the buffer size.
- **Sequencing:**
  - Keep the fence value `EndCommands` returns in `Evaluate` (2425).
  - Replace the banner branch with a new `PumpFrame()`:
    - `pump_queue->Wait(h.fence, last_eval_fence)`.
    - Barrier `PRESENT→COPY_DEST`, `CopyResource(bb, Output)`, barrier back.
    - A 3-slot allocator ring shaped like `BeginCommands` (902-951).
    - `Present(0, DO_NOT_WAIT)`.
  - Result: the evaluate list (with MGPU's vector copy) runs, then effects/tap, then `finish_effects` (MGPU colour), then `present` (MGPU signal). The overlay is drawn after capture.
- **Exactly one present per evaluate:**
  - Skip `PumpRetireOwedPresents`/debt (3311, 2820, 1694) and the no-feature presents (3163).
  - Idle re-present only while the host overlay is open, capped at 10 Hz.
  - After `WAS_STILL_DRAWING`, one `Present(0, DXGI_PRESENT_TEST)` clears ReShade's latch, behind `[DLSS5Host] MgpuLatchClear` (default decided by measurement).
- **Split `InitBanner`** (1406-1429) so the pump and panel pairs exist under `--hide`. Skip banner/panel copies (1742, 2929-2945, 3016-3044), send panel size 0 in `FeedHelloAck` (2712), and wait on the pump fence before the `'B'` teardown releases textures (2849-2855).
- **Vector state:** generalise `OptiBarriers` (2334-2358) with an input state. In frame mode, transition the vector texture `COMMON → PIXEL|NON_PIXEL` around `SafeEvaluateDLSS` (2391-2395) so MGPU's barrier is legal.
- **Ack bit:** `FEED_ACK_MGPU_FRAMES` on `FeedBuildAck::flags` (unset keeps old behaviour, same precedent as `FEED_BUILD_ASYNC_HOME`, feed_ipc.h:86-90). `dlss5-feed32.cpp` shows "MGPU Bridge (host64) is the consumer" in its overlay (5444-5452 pattern), rewords cast, and offers a "show host window" action.
- **Test switches:**
  - `--d3d12-debug` (port `FeedEnableD3D12DebugLayer`, dlss5-feed.cpp:4107).
  - `--mgpu-sim` in `RunTest`, which records MGPU's exact barrier and copy before the evaluate.
- **Window mode default** (`[DLSS5Host] MgpuWindow=hidden|behind|shown`): chosen from the B3 measurement. Pump-fence latency under `--behind` is a known stall risk (host:1456-1463).

### B4. Depth for MGPU's tap inside the host
- On each build, create an R32F SRV on `h.tex[FEED_DEPTH]` through the runtime device and call `update_texture_bindings("DEPTH", srv, srv)`.
- Rebind in `reshade_reloaded_effects`; it runs after Generic Depth because we register later.
- Unbind and destroy the view before teardown.
- If registration failed, warn to set `Depth=0` in `host64\mgpu\mgpu.ini` (never written by us).

## Phase C: upstream issue draft (local file only, not posted)
Write `UPSTREAM-MGPU-BRIDGE.md` at the repo root. It asks for:
1. A named vector input, e.g. reading `DLSS5_MV`, so no GPU-0 evaluate is needed.
2. Release or replace for latched handles past the 4-slot cap.
3. Tolerating or parameterising the vector resource state.
4. A fence signal that does not stall behind an uncomposited window.
5. Robust `ReShade2.ini` numbering alongside Smooth Motion or other swapchains.
6. An exported marker (e.g. `MGPU_ABI`) for detection.
7. Confirmation that a D3D12 swapchain created after a non-D3D12 one can still select an adapter.

## Deferred
64-bit D3D11/Vulkan/OpenGL: warning only (A2). Revisit with an in-process mirror swapchain after a dual-GPU tester confirms B.

## Verification

**Build:** run `build.bat`, `build-addon32.bat` and `host\build-host.bat` **one per call**, and check each artefact's hash changed.

**Local checks per phase (single GPU):**

| Phase | Check |
|---|---|
| P0 | Rig and in-game logs as described in P0. |
| A1 | D3D12 game with Smooth Motion on, and an HDR10 D3D12 game: the colour-space line names the game window's class; the PQ bridge decision is unchanged. |
| A2 | Fixture folders, each read from `dlss5-feed.log` and the overlay:<br>• dlssnr beside exe<br>• DepthInverted mismatch<br>• Chicken + MGPU<br>• `MvecFromEval=0`<br>• upside-down define<br>• D3D11 game + MGPU<br>Also confirm no warm-up re-create is logged when MGPU is the sole consumer. |
| A3 | Verify against MGPU-only, MGPU+Chicken and 32-bit misplaced fixtures (exit codes and messages); installer into a scratch game folder with an MGPU layout, confirming no dlssnr beside the exe. |
| B1 | Host and game LUIDs match in both logs. Force a 32-bit game onto the AMD iGPU: the host follows and fails with a clear line. `--test --adapter-luid=<5090>`. |
| B2 | Rig without the tap .fx: finish_effects=0 warning. With it: the ratios are 1.00. |
| B3 | Rig with `--d3d12-debug --mgpu-sim`: 300/300 and zero state errors. A real 32-bit D3D11 game and a dgVoodoo D3D9 game: a host64 ReShade screenshot shows the game frame at game resolution; counters 1.00. Window-mode × fullscreen/borderless/windowed matrix for p50/p99 pump-fence latency fixes the defaults. |
| B4 | `MGPU_DepthOutTex` preview in the host overlay shows scene depth; still bound after reload and after a resolution change. |

**Left for a dual-GPU tester** (listed in README and the issue):
- MGPU arms both in-process and via host64.
- The vector-copy count rises.
- NR output on GPU 1 moves correctly.
- Latency with MGPU's real fence.
- More than 4 feature creates across resize/alt-tab.
- `SRUpscale`.
- Focus with `NoActivate`.

## Critical files
- `src/dlss5-feed.cpp`
- `src/feed_mgpu.h` (new; template `src/feed_opti.h`)
- `host/dlss5-feed-host64.cpp` and `host/build-host.bat`
- `src/dlss5-feed32.cpp`
- `src/feed_ipc.h` (ack bit only)
- `tools/Verify-DLSS5Feeder.ps1` and `tools/Install-DLSS5Feeder.ps1`
- `README.md`, `DEPLOY-DEV.md`
- `UPSTREAM-MGPU-BRIDGE.md` (new)
