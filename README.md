# Card of the Day

A small **GTK4 (C)** desktop app that mints a collectible, Pokemon /
Magic‑the‑Gathering‑style **"trading card" for today's calendar date**, using a
local LLM as the "Oracle of Days".

When it opens it:

1. Detects the current calendar day (or honours a `COTD_DATE=YYYY-MM-DD`
   override for testing).
2. Asks a local LLM — **Qwen3.8‑27B** served by vLLM on `127.0.0.1:8000` — to
   recall what it knows about that day in history and whether the day is
   special in any other way.
3. The model returns a JSON "card spec": a name, an epithet, a creature‑type
   line, a rarity, a power level, five stats, flavour text, fun facts, and a
   piece of **SVG artwork** illustrating the day's theme.
4. The app renders all of that as a trading card (artwork + stats + fun facts)
   in a GTK window, with a **"⟳ Re‑roll the Oracle"** button to draw again.

---

## The original prompt (verbatim)

> Build a small GTK app that uses the Qwen3.8-27B model running right now in
> its inference paths. When the app opens it detects the current calendar day
> and recalls historical facts it knows about that day in history and whether
> the day is special in any other ways. The inference engine then builds data
> to populate a "trading card" (like a Pokemon or Magic the Gathering card)
> demonstrating the power of the day and including SVG artwork illustration,
> stats, and fun facts about the day. Record everything you do, including this
> prompt, in a README.md in the project. When you're done, open the GTK app.
> Choose any programming language you wish from Rust to Python to C++ to even
> FORTRAN.

(Also preserved verbatim in `PROMPT.md`.)

---

## Provenance

This project was **authored by an AI agent (`opencode`) driving the editor and
shell**, with the card content **generated at runtime by
`qwen/qwen3.8-27b`** running on **Tenstorrent hardware**.

| | |
|---|---|
| **Authoring agent** | `opencode` (AI coding agent) |
| **Card‑minting model** | `Qwen/Qwen3.8-27B` (served by vLLM, OpenAI‑compatible API) |
| **Inference hardware** | Tenstorrent **Quiet Box 2** — 4× Tenstorrent chips (TT‑KMD 2.11.0 driver) |
| **OS / desktop** | Ubuntu 24.04.5 LTS, KDE Plasma (Wayland), host `tsingletaryTT-quietbox` |
| **App stack** | C, GTK 4.14, cairo, librsvg 2.58, json‑glib 1.8 |
| **Inference server** | vLLM, `Qwen/Qwen3.8-27B`, `--max-model-len 262144`, FABRIC_1D, port 8000 |

The model was served locally on the Quiet Box 2's Tenstorrent accelerators
(vLLM with the TT backend, `fabric_config: FABRIC_1D`); no cloud API was
called at any point.

---

## Timing

All times local (PDT), 2026‑09‑23.

| Event | Time | Elapsed from prompt |
|---|---|---|
| Prompt received | 10:50:56 | — |
| First command run (env recon) | 10:55:09 | +4 m 13 s |
| App binary first launched (window created) | 11:29:25 | **+38 m 29 s** |
| Re‑roll crash reported & root‑caused | ~12:00–12:10 | ~+1 h 20 m |
| Fix verified (7+ clean re‑rolls) | ~12:22 | ~+1 h 31 m |
| Clean production instance launched | ~12:22 | ~+1 h 31 m |

* **Prompt → first running app: ≈ 38.5 minutes.**
* The single largest cost was iterating against a non‑standard, stripped GTK4
  build (no `gtk_run()`, no drawing‑area `draw` signal, paintable‑based
  images) and the LLM's quirks (see *Gotchas*).

---

## Tokens & cost

The model server is a **shared, long‑running vLLM instance** (started
10:07:23, before this session), so its cumulative counters include other
traffic and are **not** a clean measure of this project alone. Both views are
given below.

**Server‑lifetime counters (shared, upper bound):**

| Metric | Value |
|---|---|
| Prompt tokens processed | ~13.6 M |
| Generation tokens produced | ~198,777 |
| Total requests served | ~148 |

**This project's share (estimated):** the app and its development made on the
order of **30 LLM calls** (initial loads + re‑rolls + development tests). Each
call is ~500 prompt tokens + ~1,200 generation tokens, so this project
accounted for roughly **5×10⁴ – 1×10⁵ tokens** in total — a small fraction of
the server‑lifetime totals above.

**Cost:** the model ran **locally on the Quiet Box 2**, so the direct API cost
was **$0**. The compute cost was a few minutes of inference across 4
Tenstorrent chips (≈ 30 calls × ~10 s ≈ 5 min of model time). If the same
~10⁵ tokens were billed at a typical cloud LLM rate (order of $1–3 per
1M tokens blended), the equivalent cloud spend would be on the order of
**a few cents**.

---

## What was built

| | |
|---|---|
| **Language** | C (GTK4) |
| **Source** | `src/main.c` (single file, ~950 lines) |
| **Build** | `make` → `./card-of-the-day` |
| **LLM** | `Qwen/Qwen3.8-27B` via vLLM OpenAI‑compatible API on `127.0.0.1:8000` |
| **Artwork** | Model‑generated SVG, rasterised with librsvg |
| **JSON** | parsed with json‑glib |

### Why C / GTK4?
The prompt allowed any language "from Rust to Python to C++ to even FORTRAN".
C + GTK4 was chosen because GTK4 is the native toolkit for the target desktop,
needs no interpreter/runtime to ship, and compiles to a single small binary.

---

## How it works

```
┌────────────┐   HTTP POST (curl)   ┌────────────────────────┐
│  GTK4 app  │ ────────────────────> │ vLLM  :8000           │
│  (C)       │                       │ Qwen3.8-27B (TT hw)   │
│            │ <──────────────────── │ (OpenAI‑compatible)   │
└────────────┘   chat completion JSON└────────────────────────┘
     │
     │  parse JSON  →  CardData {name, title, type, rarity,
     │                  power, 5 stats, flavor, facts, svg}
     ▼
  render card:  SVG artwork (librsvg→cairo→texture)
                + stat bars (GtkLevelBar)
                + labels (name/rarity/type/flavor/facts/footer)
```

* **HTTP** — the request is shelled out to `curl` via `g_spawn_sync` (robust
  across this box's network stack) rather than hand‑rolling a socket client.
* **Artwork** — the model returns an SVG string; it is rasterised with
  **librsvg** into a cairo surface → pixbuf → `GdkTexture` and shown in a
  `GtkImage`.
* **Stats** — five stats (Historical Weight, Cultural Resonance, Scientific
  Impact, Chaos Factor, Vibes) are drawn as `GtkLevelBar` bars.
* **Fallback** — if the LLM is unreachable or returns unparseable output, a
  built‑in "The Uncharted Day" fallback card is shown so the app always shows
  something.

### The LLM prompt
The app sends a system prompt ("You are the Oracle of Days…") plus a user
prompt that pins the exact JSON schema to return (see `build_request_body()`
in `src/main.c`). Two non‑obvious requirements were discovered empirically
(see *Gotchas*):

* `chat_template_kwargs: {"enable_thinking": false}` — **required**, otherwise
  the model spends its whole token budget on hidden reasoning and returns an
  empty `content`.
* `response_format` **cannot** be set — this model rejects it (it is a
  "block‑output" model), so JSON discipline is enforced by the prompt alone.

---

## Build & run

```sh
make                 # builds ./card-of-the-day
./card-of-the-day     # opens the window, fetches today's card
```

Useful environment variables:

| Var | Effect |
|---|---|
| `COTD_DATE=YYYY-MM-DD` | Mint a card for this date instead of today. |
| `COTD_AUTO_REROLL_SECONDS=N` | Debug aid: auto re‑roll N seconds after each card appears. |

Dependencies (all present on this box): `gtk4`, `librsvg-2.0`,
`json-glib-1.0` (dev packages) and `curl` on `PATH`.

---

## Environment gotchas (this box is unusual)

1. **No `gtk_run()`.** This GTK build has no `gtk_run()`; the main loop is
   driven with an explicit `GMainLoop`.
2. **No `draw` signal on `GtkDrawingArea`.** Custom drawing via a drawing‑area
   signal is not possible here, so stat bars use `GtkLevelBar`.
3. **Removing a widget from its container destroys it** (the container is the
   sole owner). Never re‑parent a previously‑removed widget — build fresh
   widgets. (This was the cause of the re‑roll crash; see *Bug log*.)
4. **`rsvg_handle_new_from_data()` needs an explicit byte length** (not `-1`).
5. **No `gtk_image_new_from_gdk_texture()`.** Images are attached via
   `gtk_image_new_from_paintable()` (a `GdkTexture` is a `GdkPaintable`).
6. **The LLM rejects `response_format`** and **requires**
   `chat_template_kwargs={"enable_thinking": false}`.
7. **Screenshots are unreliable here.** `scrot`/`grim`/`import` all misbehave
   under this Wayland/KWin session (all‑black frames, `wlr-screencopy`
   unsupported, KWin D‑Bus screenshot returns "not authorized"). Verify UI
   state via `xdotool search` and app logs instead.

---

## Bug log

* **Crash on "Re‑roll the Oracle" (fixed).**
  *Symptom:* clicking *Re‑roll the Oracle* crashed the app
  (`invalid unclassed pointer in cast to 'GtkSpinner'`,
  `GTK_IS_WIDGET (child)` assertion).
  *Cause:* the loader widget and its spinner were created once and stored in
  globals. When the first card was presented, the loader was removed from its
  container — which, in this GTK build, **destroyed** it — leaving the globals
  as dangling pointers. The next re‑roll re‑added the freed widget → crash.
  *Fix:* build a **fresh** loader widget inside `start_fetch()` on every fetch
  instead of re‑using a persistent one. Verified with 7+ consecutive re‑rolls,
  zero crashes.

---

## Session log (condensed)

Full machine‑readable timeline lives in `CLAUDE.md`.

* **10:50** — Prompt received.
* Recon: found vLLM serving `Qwen/Qwen3.8-27B` on `:8000`; confirmed the
  OpenAI‑compatible API, the `enable_thinking` requirement, and that
  `response_format` is rejected.
* Chose **C + GTK4** after probing the environment (GTK4 4.14 present; no
  PyGObject; cargo present but C was the most direct fit).
* Wrote `src/main.c` + `Makefile`. Iterated on the non‑standard GTK build
  (no `gtk_run`, no drawing‑area `draw` signal, paintable‑based images).
* **11:29** — First launch (~38.5 min after the prompt).
* User reported a crash on re‑roll → reproduced under gdb with a
  `COTD_AUTO_REROLL_SECONDS` debug trigger → root‑caused to a dangling loader
  widget → fixed by allocating a fresh loader per fetch. Verified with
  repeated auto re‑rolls.
* **~12:22** — Clean production instance launched.
