# Draft: issue for maohgad-web/Neural-coprocessor (MGPU Bridge)

**Local draft only. Not posted.** Written 2026-09-17 against MGPU Bridge 0.2.3 (`de8bf97`), from
reading its source and from what its calibrator logs on a single-GPU machine. Nothing here has
been observed on two GPUs.

---

**Title:** Pairing MGPU Bridge with DLSS5-Feeder (games without DLSS): what works today, and seven small asks

Hello — DLSS5-Feeder (which your ACKNOWLEDGEMENTS already mention, thank you) builds a synthetic
DLAA call for games that have no DLSS: a feature-1 `CreateFeature`, then one `EvaluateFeature` per
frame with validated optical-flow motion vectors (R16G16_FLOAT, full resolution, `MVLowRes` set,
scale 1,1), raw depth and the back buffer. That is exactly the call your calibrator listens for, so
the two already pair up with no code on either side. The feeder now detects MGPU Bridge and
arranges itself around it (alpha):

- **64-bit D3D12 game, in-process.** The evaluate is recorded on the feeder's own list and
  submitted to the game's queue inside `reshade_render_technique`; the vector texture is a ReShade
  effect texture in `PIXEL|NON_PIXEL` shader-resource state, which is what your `hook_evaluate`
  barrier assumes. The feeder skips its warm-up re-create when you are the only consumer.
- **32-bit games (any API), through the feeder's 64-bit helper.** The helper is a D3D12 process
  with ReShade x64. With your add-on alone in its folder it resizes its swapchain to the game's
  resolution and format, copies each evaluate's output into the back buffer and presents exactly
  once per evaluate, binds the game's depth as the `DEPTH` semantic on its runtime, and transitions
  the vector texture to `PIXEL|NON_PIXEL` around the evaluate.

Measured on one RTX 5090 (so you refuse at T2, correctly): `CALIBRATOR INSTALLED … site=iat`,
`[R134] GAME CreateFeature: id=1 … the latch fires on this one`, and `[R101] … resolved=1 creates=1
sr-handle=known evaluates=N captured=N` with our table read back correctly. We cannot go further
without a second RTX card. If you (or a user of yours) can run it, the feeder's log and yours side
by side would tell us everything.

Seven things that would make this robust rather than incidental. None is urgent.

1. **A named vector input.** Today the only lane is the evaluate tap, so a feeder has to run a
   real DLSS evaluate on GPU 0 just to hand you a texture. If the probe also accepted a ReShade
   effect texture by name or semantic (we publish `DLSS5_MV`, R16G16_FLOAT, pixels, plus
   `DLSS5_Depth`), GPU 0 could skip DLSS entirely.
2. **Release or replace latched handles.** `SCENE_FEATURE_SLOTS` is 4 and nothing is ever removed.
   A producer that re-creates its feature on every resolution change (we must) runs out after four,
   and from then on `is_latched_scene_handle` is false and every evaluate is skipped — silently
   from the user's side. Hooking `NVSDK_NGX_D3D12_ReleaseFeature` to drop the handle, or replacing
   the oldest, would fix it.
3. **The vector resource's state.** `hook_evaluate` hard-codes `StateBefore =
   PIXEL|NON_PIXEL`. NGX's own contract only asks for `NON_PIXEL_SHADER_RESOURCE`, and a producer
   that hands NGX a COMMON texture is legal too. (Measured: the debug layer accepts your barrier on
   a COMMON texture, because COMMON promotes to read states. A texture explicitly in plain
   `NON_PIXEL` was not tried.) An ini key, or
   reading the state from a producer-set NGX parameter, would remove the guess.
4. **A present signal that cannot stall behind an uncomposited window.** Our helper's window is
   normally parked behind a fullscreen game or hidden; DXGI then holds its presents. Anything of
   yours that waits on a fence signalled after that swapchain's present would stall with it.
5. **`ReShade2.ini` numbering.** ReShade numbers configs by swapchain creation order. NVIDIA
   Smooth Motion adds a proxy swapchain of its own, so with both present your window may get
   `ReShade3.ini`. Matching by window class instead of by number would be immune.
6. **An exported marker.** We detect you by the `NAME` export string and the file-name substring.
   An exported `MGPU_ABI` integer (and ideally a small struct: armed or not, slots used, copies
   taken) would let a producer report your state instead of grepping `ReShade.log`.
7. **A D3D12 swapchain that appears after a non-D3D12 one.** In a 64-bit D3D11 game the feeder
   could create a private D3D12 mirror swapchain for you to capture. Does your adapter selection
   still work when the first swapchain in the process was not D3D12?

Details of what the feeder does, and the rig that reproduces the numbers above without a game, are
in its `docs/DEPLOY-DEV.md` §9c and `src/feed_mgpu.h`.
