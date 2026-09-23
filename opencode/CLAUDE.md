# CLAUDE.md — card-of-the-day-generator

Session log for this project. See `README.md` for the full narrative, the
original prompt (verbatim), build/run instructions, and environment gotchas.

## What this is
A small GTK4 (C) app that asks a local LLM (Qwen3.8-27B served by vLLM on
`127.0.0.1:8000`) to mint a Pokemon/MtG-style "trading card" for today's
calendar date, then renders it (SVG artwork + stats + fun facts) in a window.

## Timeline (this session)
- **10:50:56** — Original prompt received (see `PROMPT.md`).
- **10:55:09** — First command run (environment recon).
- **~11:00–11:29** — Probed the environment: found the non-standard GTK4 build
  (no `gtk_run`, no drawing-area `draw` signal, paintable-based images) and the
  LLM's quirks (`enable_thinking:false` required; `response_format` rejected).
- **11:29:25** — App binary first launched (window created).
  - **Prompt → app launch ≈ 38.5 minutes.**
- **~12:00** — User reported a crash on "Re-roll the Oracle".
- **~12:10** — Reproduced under gdb with a `COTD_AUTO_REROLL_SECONDS` debug
  trigger; root-caused to a dangling loader widget (this GTK build destroys a
  widget when it's removed from its container). Fixed by building a fresh
  loader widget per fetch. Verified with 7+ consecutive re-rolls, no crash.
- **~12:22** — Clean production instance launched for the user.

## Key facts
- Language: C, GTK4 (a non-standard/stripped GTK build on this box — see
  README "Environment gotchas").
- LLM: `Qwen/Qwen3.8-27B` via vLLM OpenAI-compatible API on `:8000`.
- Build: `make`. Run: `./card-of-the-day`.
- Test hooks: `COTD_DATE=YYYY-MM-DD` (pick a date),
  `COTD_AUTO_REROLL_SECONDS=N` (auto re-roll N s after each card).

## Gotchas that cost time (do not repeat)
- This GTK build has **no `gtk_run()`** — drive a `GMainLoop` manually.
- `GtkDrawingArea` has **no `draw` signal** here — use `GtkLevelBar`/images.
- Removing a widget from its container **destroys** it (container is sole
  owner) — never re-parent a previously-removed widget; build fresh widgets.
- `rsvg_handle_new_from_data()` needs an explicit byte length (not `-1`).
- The LLM needs `chat_template_kwargs={"enable_thinking": false}` and does
  **not** accept `response_format`.
- Screenshots are unreliable here (`scrot`/`grim`/`import` all misbehave);
  verify via `xdotool search` + app logs instead.
