# TCP debug server

`runtime/src/debug_server.c` exposes the running game on `127.0.0.1:4370` as a
line-oriented JSON service. It is the supported way to drive a build from a
script: save/load states, step frames, inject input, capture what is on screen,
read memory, and query the always-on frame ring.

It is the GB counterpart of `nesrecomp/runner/src/debug_server.c` and
`snesrecomp/.../runner/src/debug_server.c`; command names and reply shapes match
those ecosystems where the concept is the same.

A ready-made Python client lives in each game repo as `tools/tcp.py`
(`from tcp import Debug`), which wraps everything below.

## Protocol

* One JSON object per line, `\n`-terminated, both directions. UTF-8.
* Request: `{"cmd":"<name>","id":<int>, ...args}`. `id` is echoed back; use a
  fresh one per request and match replies by it.
* Reply: `{"id":<int>,"ok":true, ...}` or `{"id":<int>,"ok":false,"error":"..."}`.
* Asynchronous **events** (`{"event":"...", ...}`) share the same stream and
  carry no `id`. A client must skip lines whose `id` does not match, and read
  forward when it is waiting for an event.
* A few commands **stream**: several reply lines with the same `id`. Those are
  called out below.
* One client at a time. The listener accepts a new client when the previous one
  disconnects; disconnecting also clears the pause state and any input override.
* Port: 4370 by default, or `$GBRECOMP_DEBUG_PORT`.
* The JSON parser is hand-written and does **not** process escapes. Send paths
  with forward slashes (`F:/Projects/...`), never backslashes.

### Frame boundaries

Commands are dispatched from `gb_debug_server_poll()`, which the platform calls
from `gb_platform_poll_events()` once per guest frame, between frames — never
from inside a recompiled body. State-mutating commands (`load_state`,
`save_state`, `write_ram`, the input commands) therefore take effect at exactly
the point the equivalent keypress would.

### Relative paths

Paths are resolved by the runner process, whose working directory is the folder
holding the executable (also where `rom.cfg`, `.sav` and `.stateN` files live).
Absolute paths are safest from a script.

---

## Execution control

| Command | Args | Reply | Notes |
|---|---|---|---|
| `ping` | — | `frame` | Connectivity check. |
| `help` | — | `commands[]` of `{name,summary}`, `docs`, `note` | Served from the dispatcher's own table, so it cannot drift from what the binary accepts. |
| `frame` | — | `frame`, `last_func` | Current guest frame and the last function the body reported. |
| `pause` | — | `paused:true`, `frame` | Halts at the next frame boundary; the window keeps pumping events. |
| `continue` | — | `paused:false` | Resumes; also clears a pending `step` / `run_to_frame`. |
| `step` | `count` (int, default 1) | `stepping` | Runs `count` frames then re-pauses. Emits `step_done`. |
| `run_to_frame` | `frame` (int) | `running_to` | Resumes and pauses at that absolute frame. Emits `run_to_done`. Errors `target frame already passed` if it is in the past. |
| `history` | — | `count`, `oldest`, `newest` | Window of the always-on frame ring. |
| `quit` | — | `ok` | Replies, flushes, then `exit(0)`. |

## Save states

Both commands take **either** `path` **or** `slot`. A `slot` resolves through
`gb_platform_savestate_slot_path()` — the exact file the in-game save/load keys
use, `<save_id>.state<slot+1>` beside the executable (e.g. slot 0 of the SML2 DX
body is `Super_Mario_Land_2_DX.state1`). A state the user saved by hand
therefore loads over TCP and vice versa.

| Command | Args | Reply | Notes |
|---|---|---|---|
| `save_state` | `path` (str) or `slot` (int, 0-based) | `path`, `frame` | `gb_context_save_state_file()`. |
| `load_state` | `path` (str) or `slot` (int, 0-based) | `path`, `frame` | Runs the same post-load hooks as the in-game load. |
| `save_slot_path` | `slot` (int, default 0) | `slot`, `path`, `exists` | Resolve a slot file without touching it. |

`load_state` post-load hooks, in order: `gb_ws_reapply()` and `gb_custom_reset()`
inside `gb_context_load_state_file()`, so the widescreen sidecar and any custom
compositor rebuild from the restored timeline rather than the abandoned one;
then the audio output ring reset, the cached guest-framebuffer invalidation and
the present-counter resync in `gb_platform_load_state_path()`. Skipping the
custom reset leaves the compositor deriving margins from the old timeline and
the next composed frame is wrong.

## Rewind

| Command | Args | Reply | Notes |
|---|---|---|---|
| `rewind` | `frames` (int, default 0) | `frames`, `enabled`, `states`, `used`, `capacity`, `state_size` | Holds the Rewind key for the next `frames` frames, then lets go; `step` runs them. `0` only reports the buffer. |

Each held frame loads the newest state in the rewind buffer and runs the frame
from it, as holding the key does; the first also passes over the state of the
frame on screen, so it shows the one before. The frame runs with the current
input: to land exactly on an earlier frame of a scripted run, `set_input` that
frame's buttons before the last rewind step. `used` is bytes of patches (one per
state after the newest, which is kept whole), `capacity` is 0 until the buffer
exists, and `state_size` is the whole state's size. Rewind runs from
`gb_platform_vsync()`, so `--benchmark` runs have none.

## Speed

| Command | Args | Reply | Notes |
|---|---|---|---|
| `speed` | `fast_forward`, `max_speed`, `vsync` (int 0/1 each), `percent` (int 10-500), `fast_forward_percent`, `max_percent` (int 110-1000, 0 = Unlimited); absent leaves it | `effective_percent`, `fast_forward_percent`, `max_percent`, `guest_fps`, `fast_forward`, `max_speed`, `vsync`, `swap_interval`, `audio_mode`, `audio_step`, `present_ms`, `frameskip`, `frames_skipped` | Holds Fast Forward (Hold) (until `fast_forward:0`) and sets Fast Forward (the toggle, once called Max Speed, hence `max_speed` / `max_percent`), the V-Sync setting, the menu's Speed % and the shortcuts' speeds, as the keys and menu do (the speeds are not saved). No args only reports. |

The shortcuts' speeds are the `speed.fast_forward_percent` and
`speed.max_percent` prefs, where 0 is Unlimited: no frame limiter at all
(RetroArch's Fast-Forward Rate 0), and `effective_percent` is 0 while it is in
effect. `guest_fps` is the game frames run per second, measured twice a
second. `swap_interval` is what presents use now: V-Sync is off above 100%.
`audio_mode` is the `audio.fast_forward` pref (0 mute, 1 normal pitch, 2 sped
up) and `audio_step` the game samples per sample played that the sound is
resampled with, measured from the real frame times; `present_ms` is the last
present, V-Sync wait included. `frameskip` is the
`speed.fast_forward_frameskip` pref (RetroArch's Fast-Forward Frame Skip, on
by default): above 100% a frame is drawn only once a display refresh period
has passed since the last one. `frames_skipped` counts the frames it has not
drawn since launch; they still run and count as frames. The skip needs a GL
window, so `--benchmark` runs draw every frame.

| Command | Args | Reply | Notes |
|---|---|---|---|
| `window` | `width`, `height` (int, together), `scaling_mode` (int 0-3); absent leaves it | `window_width`, `window_height`, `view_width`, `view_height`, `native_presented`, `picture_width`, `picture_height`, `game_x`, `game_y`, `game_width`, `game_height`, `scaling_mode`, `fullscreen` | Resizes the window while windowed (or the windowed size a headless run resolves against) and sets the scaling mode, without saving either. `view_*` is the view of the last presented frame: a custom view that fills the window (`gb_custom_requested_width` -1) resolves the new size on the next present, so `step` before reading it back. `native_presented` is true when that frame was a custom view's native 160x144 picture presented on its own (`gb_custom_native_scaling`; `screenshot` is then 160x144), `picture_*` is the size of the picture presented (the view, the part of it a `gb_custom_fit` hook kept, or 160x144) and `game_*` its rect in the window. |

## Input

Button arguments accept two spellings, interchangeably:

* **letters** — `R L U D A B S T`, where `S` is Start and `T` is selecT; `-` or
  `""` means nothing pressed. Same letters as the `--input` script route and
  `platform_sdl.cpp`'s `parse_buttons()` / `write_buttons()`.
* **hex mask** — `"0x30"` or `"30"`; bit 0 R, 1 L, 2 U, 3 D, 4 A, 5 B, 6 Select,
  7 Start, active high.

While an override is active it replaces the real joypad entirely — it is applied
at the end of `gb_platform_poll_events()`, on top of keyboard, controller, the
`--input` script and the in-game menu gate, and newly pressed buttons raise the
joypad interrupt the same way a real press does. Clearing it hands control back.
All five commands reply with the resulting state: `cmd`, `buttons`
(letters), `mask` (int, `-1` when no override), `frames` (remaining transient
frames, 0 = held) and `frame`.

| Command | Args | Notes |
|---|---|---|
| `press` | `buttons`, `frames` (int, default 1) | Transient: held for `frames` **guest** frames, then released automatically. The countdown advances in the per-frame record hook, so it only runs while the game runs — issue `press`, then `step`/`run_to_frame`. |
| `hold` | `buttons` | Adds buttons to the held mask. |
| `release` | `buttons` | Removes buttons; releasing the last one clears the override entirely. |
| `set_input` | `buttons` | Sets the whole held mask absolutely (not incremental). |
| `clear_input` | — | Drops the override. |

## Menus

The runtime menu (Escape) and the settings window. While either is open the
game gets no input and no shortcut fires; with Pause in Menu on (the
`ui.pause_in_menu` pref, default 1) the game also waits in
`gb_platform_vsync`, presenting its last frame, so `step` and `run_to_frame`
do not finish until the menu closes.

| Command | Args | Reply | Notes |
|---|---|---|---|
| `menu` | `open` (`main`, `settings`, `shaders`: the settings window at Shader Presets, `none`), `pause_in_menu` (int 0/1), `dim_percent`, `opacity_percent` (int 0-100), `leave` (`quit` or `launcher`); absent leaves it | `main_open`, `settings_open`, `game_held`, `pause_in_menu`, `dim_percent`, `opacity_percent` | Opens or closes the menus and sets Pause in Menu, Game Dimming and Menu Opacity without saving. `leave` does what the menus' Quit / Return to Launcher do (the latter errors where there is no launcher to return to). |
| `ui_event` | `key` (SDL key name: `Escape`, `P`, `Return`, `Down`) with `down` (1 press, 0 release, absent both); `text` (typed characters); `x`, `y` (window coordinates) with `button` (1 left) and `down` | `ok` | Window input as the user's, for testing the menus: it goes through the same handling (ImGui, the menus' Escape / Back, the shortcuts). Queued and handed to SDL where the game polls its own events: the menus' hold polls all along, a `pause`d runner once it runs again (the pause loop drains SDL's queue itself). |
| `window_screenshot` | `path` (str) | `ok` | Writes the next presented window as PNG: the menus and the shader preset included, unlike `screenshot`. Frames need not advance; a menu keeps presenting while it holds the game. |
| `restart` | — | `ok` | Restart Game: the machine goes back to how it was before its first frame (kept by `gb_before_first_frame`) at the next frame boundary, with the cart's battery RAM as it is now, flushed to disk first. Errors before the first frame has started. |

## Screen capture

| Command | Args | Reply | Notes |
|---|---|---|---|
| `screenshot` | `path` (str, default `gb_shot_<frame>.ppm`), `recompose` (int, default 0) | `path`, `width`, `height`, `frame`, `source` | Writes **what the user sees**. |

* Source is the *presented* frame: the composited custom/wide frame when a
  compositor is installed (`gb_custom_render`, width `gb_custom_width`),
  otherwise the native 160x144 framebuffer. `source` is `"presented"` when it
  came from the platform's present path and `"composed"` when it was built here
  (headless run, or before the first present).
* Format follows the suffix: `.png` (case-insensitive) writes a real PNG via the
  vendored `stb_image_write`; anything else writes binary PPM (P6).
* `recompose:1` re-runs the compositor against current VRAM/OAM instead of
  reusing the last presented frame. The default answers "what is on screen".

## Memory

| Command | Args | Reply | Notes |
|---|---|---|---|
| `read_ram` | `addr` (hex str), `len` (int, default 1, clamped 1-256) | `addr`, `len`, `hex` | Goes through `gb_read8` — sees the live bank and any custom read override, and can trip watchpoints. |
| `dump_ram` | `addr` (hex str), `len` (int, default 256, clamped 1-8192) | **streams** `addr`, `offset`, `len`, `hex` per 256-byte chunk | Same read path as `read_ram`. |
| `peek` | `addr` (hex str), `len` (default 256, clamped 1-8192), `rom_bank`, `ram_bank`, `wram_bank`, `vram_bank` (int, default -1 = live bank) | **streams** `addr`, `offset`, `len`, `total`, `hex` | Reads the backing arrays directly: bypasses `gb_read8`, watchpoints and custom read overrides, and can name a bank that is not currently mapped. `SVBK 0` aliases WRAM bank 1. |
| `poke` | `addr` (hex str), `hex` (bytes), `ram_bank`, `wram_bank`, `vram_bank` (int, default -1 = live bank) | `ok` | `peek`'s writing twin for RAM (VRAM, cartridge RAM, WRAM and its extension, OAM, HRAM; not ROM or I/O): writes the backing arrays with no side effects. `write_ram` goes through the bus, which refuses everything but HRAM during OAM DMA, where a pause often lands. |
| `write_ram` | `addr` (hex str) plus `hex` (byte run) or `val` (single byte) | `ok` | Debug poke through `gb_write8`. |
| `read_vram` | `addr` (hex str), `len` (default 16, clamped 1-256) | `addr`, `len`, `hex` | 0x8000-0x9FFF only; bytes outside read as 0. Current VRAM bank only — use `peek` for a specific bank. |
| `read_oam` | `index` (int, default -1) | with a valid index: `index`, `y`, `x`, `tile`, `flags`; otherwise `count`, `hex` (all 160 bytes) | |
| `read_io` | `addr` (hex str), `len` (default 1, clamped 1-128) | `addr`, `len`, `hex` | 0xFF00-0xFF7F only. |

## State inspection

| Command | Args | Reply | Notes |
|---|---|---|---|
| `get_registers` | — | `A`,`F`,`B`,`C_reg`,`D`,`E`,`H_reg`,`L`,`SP`,`PC`,`Z`,`N`,`H`,`C`,`IME`,`rom_bank`,`ram_bank`,`frame` | Flags are packed first. `C_reg`/`H_reg` are the registers; `C`/`H` are the flags. |
| `ppu_state` | — | `LCDC`,`STAT`,`SCY`,`SCX`,`LY`,`LYC`,`WY`,`WX`,`BGP`,`OBP0`,`OBP1` | Straight from the I/O block. |
| `hw_state` | — | `model`,`cgb`,`cgb_compat`,`body`,`rom_size`,`mbc`,`bgpi`,`obpi`,`bg_palette`,`obj_palette` | Palettes are 64-byte hex strings, empty on DMG. Non-mutating: reads the PPU's palette RAM rather than poking BCPS/BCPD. |
| `mapper_state` | — | `rom_bank`,`ram_bank`,`mbc_type`,`ram_enabled`,`mbc_mode` | |
| `interp_fallbacks` | — | `total_fallbacks`,`total_entries`,`total_instructions`,`total_cycles`,`frame_fallbacks`,`frame_first`,`frame_last`,`unimplemented_opcode`,`sites[]` | Always-on interpreter-fallback ring; each site is `{bank,addr,entries,instructions,cycles,last_frame}`. Site list is truncated to fit a 4 KB buffer. |

## Frame ring (always-on history)

The runtime records a compact snapshot of every frame into a fixed-size ring
(`GB_FRAME_HISTORY_CAP`) from the moment the body boots. Nothing needs arming —
query the window you care about.

| Command | Args | Reply | Notes |
|---|---|---|---|
| `get_frame` | `frame` (int) | `frame`, `cpu{...}`, `ppu{...}`, `rom_bank`, `ram_bank`, `joypad`, `cycles`, `game_data` (16-byte hex), `last_func` | Errors `frame not in buffer` / `frame record mismatch` once the slot has been overwritten. |
| `frame_range` | `start`, `end` (int) | `frames[]` of `{frame,bank,joy,game_data}` (or `{frame,available:false}`) | Max 200 frames per request. |
| `frame_timeseries` | `start`, `end` (int) | `ts[]` of `{f,a,sp,pc,lcdc,ly,scx,scy,bk,joy,cyc,gd}`, `null` where unavailable | Same 200-frame cap; compact keys for cheap polling. |

`game_data` is the 16 bytes the game module fills in `game_fill_frame_record()`.

## Watchpoints

| Command | Args | Reply | Notes |
|---|---|---|---|
| `watch` | `addr` (hex str) | `slot`, `addr` | Max 8; errors `all watchpoint slots full (max 8)`. |
| `unwatch` | `addr` (hex str) | `ok` | Errors `watchpoint not found`. |

Changes are reported asynchronously as `watchpoint` events, checked once per
frame — a value that changes and changes back within one frame is not seen.

## Events

| Event | Keys | Raised by |
|---|---|---|
| `step_done` | `frame` | `step` countdown reaching zero; the runner re-pauses. |
| `run_to_done` | `frame` | `run_to_frame` target reached; the runner re-pauses. |
| `watchpoint` | `addr`, `old`, `new`, `frame` | A watched byte changed between frames. |
| `dropped` | `messages` | The outbound queue was full and whole lines were discarded — the client is not reading fast enough. Lines are dropped whole, so the stream stays newline-synchronized. |

---

## Game-specific commands

Unknown commands fall through to `game_handle_debug_cmd()` (see
`runtime/include/game_extras.h`), so a game module can add its own. Those are
documented in the game repo — for example `sml2_mod_state`, `sml2_view`,
`sml2_gate_log` in Super Mario Land 2.

A game module that shipped its own save/load/capture command before the generic
ones existed should forward rather than keep a second implementation:

```c
int gb_debug_server_save_state(int id, const char *json);
int gb_debug_server_load_state(int id, const char *json);
int gb_debug_server_screenshot(int id, const char *json);
```

Each sends the standard reply for `id` and returns 1, so a game handler reads
`return gb_debug_server_screenshot(id, "{\"path\":\"logs/probe.ppm\"}");`.
SML2's `sml2_save` / `sml2_load` / `sml2_capture` are exactly that — deprecated
aliases that pin their historic default paths.

## Client library

`tools/tcp.py` in a game repo:

```python
from tcp import Debug

with Debug(port=4370) as d:
    d.pause()
    d.load_state(path="F:/Projects/.../dx_pause_pipe_repro.state1")
    d.advance(4)
    d.screenshot("logs/before.png")
    d.press("S")                  # Start, one guest frame
    d.advance(30)
    d.screenshot("logs/after.png")
```
