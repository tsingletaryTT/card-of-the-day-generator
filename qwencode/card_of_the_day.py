#!/usr/bin/env python3
"""
Card of the Day — a GTK4 trading-card generator powered by Qwen3.8-27B.

On launch the app:
  1. Detects the current calendar day.
  2. Asks the locally-running Qwen3.8-27B model (served by vLLM at
     http://127.0.0.1:8000, OpenAI-compatible API) to "forge" a collectible
     trading card for that day: name, rarity, stats, fun facts, flavor text
     and a piece of SVG artwork.
  3. Renders the result as a Pokemon/MtG-style trading card in a GTK4 window.

Instrumentation: every run writes a timing/token report to
``timing_report.json`` next to this file (time-to-first-paint, LLM latency,
prompt/completion token counts, ...).

If the model is unreachable or returns unparseable output, a deterministic
fallback card is shown so the app always displays something.
"""

from __future__ import annotations

import json
import math
import os
import re
import sys
import threading
import time
import urllib.request
from dataclasses import dataclass, field
from datetime import date, datetime, timezone

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Rsvg", "2.0")
from gi.repository import Gtk, Gdk, GLib, Gio, Pango, PangoCairo, Rsvg  # noqa: E402

import cairo  # noqa: E402

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

API_URL = os.environ.get("COTD_API_URL", "http://127.0.0.1:8000/v1/chat/completions")
MODEL = os.environ.get("COTD_MODEL", "Qwen/Qwen3.8-27B")
REQUEST_TIMEOUT = 300  # seconds; a 27B model on TT hardware can be slow

CARD_W, CARD_H = 500, 700  # logical card size (points)

HERE = os.path.dirname(os.path.abspath(__file__))
TIMING_REPORT_PATH = os.path.join(HERE, "timing_report.json")

RARITY_COLORS = {
    "common": "#9ca3af",
    "uncommon": "#4ade80",
    "rare": "#60a5fa",
    "epic": "#c084fc",
    "legendary": "#fbbf24",
}
DEFAULT_RARITY = "common"

STAT_KEYS = ["power", "wisdom", "wonder", "chaos", "luck"]


# ---------------------------------------------------------------------------
# Timing / token instrumentation
# ---------------------------------------------------------------------------


class TimingLog:
    """Records named timestamps (monotonic + wall clock) and token usage."""

    def __init__(self):
        self.t0 = time.monotonic()
        self.started_wall = datetime.now(timezone.utc).isoformat()
        self.events: list[dict] = []
        self.token_usage: dict = {}
        self._lock = threading.Lock()

    def mark(self, name: str) -> float:
        t = time.monotonic() - self.t0
        with self._lock:
            self.events.append(
                {
                    "event": name,
                    "t_offset_s": round(t, 3),
                    "wall_clock_utc": datetime.now(timezone.utc).isoformat(),
                }
            )
        return t

    def set_token_usage(self, usage: dict) -> None:
        # Accumulate across LLM calls (a card may take more than one call if a
        # retry is needed), so the reported total reflects the whole forge.
        with self._lock:
            for key, value in (usage or {}).items():
                if isinstance(value, (int, float)):
                    self.token_usage[key] = self.token_usage.get(key, 0) + value

    def report(self) -> dict:
        with self._lock:
            return {
                "started_utc": self.started_wall,
                "events": list(self.events),
                "token_usage": dict(self.token_usage),
            }

    def write_report(self, path: str, extra: dict | None = None) -> None:
        report = self.report()
        if extra:
            report.update(extra)
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(report, f, indent=2)
        os.replace(tmp, path)


TIMING = TimingLog()
TIMING.mark("app_process_start")


# ---------------------------------------------------------------------------
# Card data model
# ---------------------------------------------------------------------------


@dataclass
class CardData:
    name: str
    subtitle: str
    rarity: str
    stats: dict  # {"power": int, "wisdom": int, ...}
    fun_facts: list
    flavor: str
    svg: str  # SVG artwork markup (may be empty -> procedural fallback art)
    date_label: str
    today: date
    source: str = "Qwen3.8-27B"  # "Qwen3.8-27B" or "fallback (...)"


# ---------------------------------------------------------------------------
# LLM interaction
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = (
    "You are the Card Forge, an oracle that forges collectible trading cards "
    "for calendar days. You always respond with a single valid JSON object "
    "and nothing else: no markdown, no code fences, no commentary."
)


def build_user_prompt(today: date) -> str:
    return f"""Today is {today.strftime('%A, %B %d, %Y')}.

Forge a collectible trading card for this calendar day. Respond with a SINGLE
JSON object (no markdown, no code fences) with EXACTLY these keys:

- "name": short evocative card name, max 4 words
- "subtitle": one-line subtitle, max 8 words
- "rarity": one of "common", "uncommon", "rare", "epic", "legendary"
  (choose by how historically/culturally significant this day is)
- "stats": an object with integer values 0-100 for exactly these keys:
  "power", "wisdom", "wonder", "chaos", "luck"
- "fun_facts": an array of exactly 3 short factual sentences (max 90 chars
  each) about this calendar day in history, culture or science. They must be
  true.
- "flavor": one evocative sentence, max 120 characters
- "artwork": an object with:
  - "title": max 40 characters
  - "svg": a complete standalone SVG document as a string, with these
    requirements:
    * starts with <svg ... viewBox="0 0 400 250" ...>
    * flat-illustration style, bold simple shapes, at most 6 colors
    * no external references, no scripts, no <text> elements
    * depicts the theme of this day

Return only the JSON object."""


def call_llm(messages: list, max_tokens: int = 8192, temperature: float = 0.8):
    """Call the local vLLM OpenAI-compatible endpoint.

    Returns (content_text, usage_dict).
    """
    TIMING.mark("llm_request_start")
    payload = {
        "model": MODEL,
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
        "top_p": 0.95,
    }
    req = urllib.request.Request(
        API_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception:
        TIMING.mark("llm_request_failed")
        raise
    TIMING.mark("llm_response_received")
    usage = data.get("usage") or {}
    TIMING.set_token_usage(
        {
            "prompt_tokens": usage.get("prompt_tokens"),
            "completion_tokens": usage.get("completion_tokens"),
            "total_tokens": usage.get("total_tokens"),
        }
    )
    message = data["choices"][0]["message"]
    return message.get("content") or "", usage


def extract_json_object(text: str) -> dict:
    """Extract a JSON object from a model response, tolerating fences/prose."""
    text = text.strip()
    m = re.search(r"```(?:json)?\s*(\{.*\})\s*```", text, re.S)
    if m:
        text = m.group(1)
    else:
        start, end = text.find("{"), text.rfind("}")
        if start != -1 and end > start:
            text = text[start : end + 1]
    return json.loads(text)


def normalize_card(raw: dict, today: date) -> CardData:
    """Coerce a parsed JSON object into a validated CardData."""
    stats = {}
    for key in STAT_KEYS:
        try:
            stats[key] = max(0, min(100, int(round(float(raw.get("stats", {}).get(key, 50))))))
        except (TypeError, ValueError):
            stats[key] = 50
    rarity = str(raw.get("rarity", DEFAULT_RARITY)).lower().strip()
    if rarity not in RARITY_COLORS:
        rarity = DEFAULT_RARITY
    facts = [str(f).strip() for f in raw.get("fun_facts", []) if str(f).strip()][:4]
    artwork = raw.get("artwork") or {}
    svg = artwork.get("svg", "") if isinstance(artwork, dict) else ""
    return CardData(
        name=str(raw.get("name", "Unknown Day")).strip()[:60],
        subtitle=str(raw.get("subtitle", "")).strip()[:90],
        rarity=rarity,
        stats=stats,
        fun_facts=facts or ["A quiet day in the calendar."],
        flavor=str(raw.get("flavor", "")).strip()[:200],
        svg=svg if isinstance(svg, str) else "",
        date_label=today.strftime("%B %d, %Y"),
        today=today,
        source="Qwen3.8-27B",
    )


def forge_card(today: date) -> CardData:
    """Ask the model to forge the card; retry once on unparseable output."""
    user_prompt = build_user_prompt(today)
    last_error = None
    for attempt in range(2):
        messages = [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_prompt},
        ]
        if attempt > 0:
            messages.append(
                {
                    "role": "user",
                    "content": (
                        "Your previous reply was not valid JSON. Respond again "
                        "with ONLY the JSON object, no markdown, no commentary."
                    ),
                }
            )
        content, _usage = call_llm(messages)
        try:
            raw = extract_json_object(content)
            return normalize_card(raw, today)
        except (json.JSONDecodeError, ValueError, KeyError, TypeError) as exc:
            last_error = exc
    raise RuntimeError(f"Model returned unparseable card data: {last_error}")


def fallback_card(today: date, reason: str = "") -> CardData:
    """Deterministic offline card used when the model is unreachable."""
    ordinal = today.toordinal()
    stats = {k: (ordinal * (i + 7) * 2654435761) % 101 for i, k in enumerate(STAT_KEYS)}
    return CardData(
        name="The Unreached Day",
        subtitle="A quiet day in the calendar",
        rarity="common",
        stats=stats,
        fun_facts=[
            f"Today is {today.strftime('%B %d, %Y')}, day {ordinal} of the calendar.",
            "The oracle was unreachable, so this card was forged locally.",
            "Stats were derived deterministically from the date itself.",
        ],
        flavor="Even a silent oracle keeps the calendar turning.",
        svg="",
        date_label=today.strftime("%B %d, %Y"),
        today=today,
        source=f"fallback ({reason[:60]})" if reason else "fallback",
    )


# ---------------------------------------------------------------------------
# SVG artwork
# ---------------------------------------------------------------------------


def render_svg_to_surface(svg_text: str, width: int, height: int):
    """Render an SVG string into a cairo ImageSurface of the given size."""
    handle = Rsvg.Handle.new_from_data(svg_text.encode("utf-8"))
    if handle is None:
        raise ValueError("Rsvg could not parse the SVG")
    iw, ih = handle.get_intrinsic_size_in_pixels()
    if not iw or not ih:
        raise ValueError("SVG has no intrinsic size")
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, width, height)
    cr = cairo.Context(surface)
    scale = min(width / iw, height / ih)
    cr.translate((width - iw * scale) / 2, (height - ih * scale) / 2)
    cr.scale(scale, scale)
    handle.render_cairo(cr, None)
    return surface


def draw_procedural_art(cr, x, y, w, h, today: date):
    """Deterministic fallback artwork: dusk sky, sun, mountains, date."""
    cr.save()
    cr.save()
    cr.rectangle(x, y, w, h)
    cr.clip()

    sky = cairo.LinearGradient(0, y, 0, y + h)
    sky.add_color_stop_rgb(0.0, 0.16, 0.12, 0.30)
    sky.add_color_stop_rgb(0.6, 0.45, 0.25, 0.45)
    sky.add_color_stop_rgb(1.0, 0.85, 0.45, 0.25)
    cr.set_source(sky)
    cr.rectangle(x, y, w, h)
    cr.fill()

    cr.set_source_rgb(1.0, 0.85, 0.5)
    cr.arc(x + w * 0.5, y + h * 0.42, min(w, h) * 0.18, 0, 2 * math.pi)
    cr.fill()

    cr.set_source_rgb(0.10, 0.08, 0.20)
    cr.move_to(x, y + h)
    cr.line_to(x + w * 0.25, y + h * 0.55)
    cr.line_to(x + w * 0.5, y + h * 0.85)
    cr.line_to(x + w * 0.72, y + h * 0.5)
    cr.line_to(x + w, y + h * 0.9)
    cr.line_to(x + w, y + h)
    cr.close_path()
    cr.fill()

    cr.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
    cr.set_font_size(min(w, h) * 0.11)
    cr.set_source_rgb(1, 1, 1)
    text = today.strftime("%b %d")
    ext = cr.text_extents(text)
    cr.move_to(x + (w - ext.width) / 2 - ext.x_bearing, y + h * 0.88 - ext.height / 2)
    cr.show_text(text)
    cr.restore()
    cr.restore()


# ---------------------------------------------------------------------------
# Card rendering (Cairo)
# ---------------------------------------------------------------------------


def _rounded_rect(cr, x, y, w, h, r):
    cr.new_sub_path()
    cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    cr.arc(x + r, y + r, r, math.pi, 1.5 * math.pi)
    cr.close_path()


def _hex_to_rgb(hexstr):
    hexstr = hexstr.lstrip("#")
    return tuple(int(hexstr[i : i + 2], 16) / 255 for i in (0, 2, 4))


def _set_text(cr, text, x, y, size, color, bold=False, italic=False, width=None, align="left"):
    fd = Pango.FontDescription()
    fd.set_family("Sans")
    fd.set_absolute_size(size * Pango.SCALE)
    if bold:
        fd.set_weight(Pango.Weight.BOLD)
    if italic:
        fd.set_style(Pango.Style.ITALIC)
    layout = PangoCairo.create_layout(cr)
    layout.set_font_description(fd)
    if width:
        layout.set_width(int(width * Pango.SCALE))
        layout.set_wrap(Pango.WrapMode.WORD)
        if align == "center":
            # Center the text *within* the fixed-width block, and center the
            # block itself on the card.
            layout.set_alignment(Pango.Alignment.CENTER)
            x = (CARD_W - width) / 2
    layout.set_text(text, -1)
    tw, th = layout.get_pixel_size()
    if align == "center" and not width:
        x = (CARD_W - tw) / 2
    cr.move_to(x, y)
    cr.set_source_rgb(*_hex_to_rgb(color))
    PangoCairo.show_layout(cr, layout)
    return th


def _draw_card_body(cr, card: CardData, art_surface):
    """Draw the card in a 500x700 coordinate space."""
    accent = RARITY_COLORS.get(card.rarity, RARITY_COLORS[DEFAULT_RARITY])
    accent_rgb = _hex_to_rgb(accent)

    # background
    bg = cairo.LinearGradient(0, 0, 0, CARD_H)
    bg.add_color_stop_rgb(0.0, 0.14, 0.11, 0.24)
    bg.add_color_stop_rgb(1.0, 0.06, 0.05, 0.12)
    _rounded_rect(cr, 0, 0, CARD_W, CARD_H, 18)
    cr.set_source(bg)
    cr.fill_preserve()
    cr.set_line_width(3)
    cr.set_source_rgb(*accent_rgb)
    cr.stroke()

    # inner hairline
    _rounded_rect(cr, 8, 8, CARD_W - 16, CARD_H - 16, 12)
    cr.set_line_width(1)
    cr.set_source_rgba(*accent_rgb, 0.5)
    cr.stroke()

    # header: name + subtitle
    _set_text(cr, card.name.upper(), 0, 18, 21, "#f8fafc", bold=True, align="center")
    if card.subtitle:
        _set_text(cr, card.subtitle, 0, 48, 11, "#94a3b8", italic=True, align="center")

    # artwork window
    art_x, art_y, art_w, art_h = 24, 74, CARD_W - 48, 240
    cr.save()
    _rounded_rect(cr, art_x, art_y, art_w, art_h, 8)
    cr.clip()
    if art_surface is not None:
        cr.set_source_surface(art_surface, art_x, art_y)
        cr.paint()
    else:
        draw_procedural_art(cr, art_x, art_y, art_w, art_h, card.today)
    cr.restore()
    _rounded_rect(cr, art_x, art_y, art_w, art_h, 8)
    cr.set_line_width(1.5)
    cr.set_source_rgb(*accent_rgb)
    cr.stroke()

    # type line
    type_line = f"DAY  •  {card.date_label.upper()}"
    _set_text(cr, type_line, 0, 326, 11, "#94a3b8", bold=True, align="center")

    # stats
    y = 356
    for key in STAT_KEYS:
        value = card.stats.get(key, 0)
        _set_text(cr, key.upper(), 28, y + 1, 11, "#cbd5e1", bold=True)
        _rounded_rect(cr, 120, y, 280, 14, 7)
        cr.set_source_rgb(0.16, 0.15, 0.24)
        cr.fill()
        fill_w = max(8.0, 280.0 * value / 100.0)
        _rounded_rect(cr, 120, y, fill_w, 14, 7)
        grad = cairo.LinearGradient(120, 0, 120 + fill_w, 0)
        grad.add_color_stop_rgb(0.0, *accent_rgb)
        grad.add_color_stop_rgb(1.0, *tuple(min(1.0, c * 1.5) for c in accent_rgb))
        cr.set_source(grad)
        cr.fill()
        _set_text(cr, str(value), 452, y + 1, 11, "#e2e8f0", bold=True)
        y += 26

    # fun facts
    facts_y = y + 8
    _set_text(cr, "FUN FACTS", 28, facts_y, 10, accent, bold=True)
    facts_y += 18
    for fact in card.fun_facts[:3]:
        th = _set_text(cr, f"•  {fact}", 28, facts_y, 10.5, "#cbd5e1", width=CARD_W - 56)
        facts_y += th + 5

    # flavor + footer
    cr.set_source_rgb(0.4, 0.4, 0.5)
    cr.set_line_width(1)
    cr.move_to(24, CARD_H - 64)
    cr.line_to(CARD_W - 24, CARD_H - 64)
    cr.stroke()

    _set_text(
        cr,
        f"“{card.flavor}”",
        0,
        CARD_H - 58,
        11,
        "#e2e8f0",
        italic=True,
        width=CARD_W - 80,
        align="center",
    )
    _set_text(
        cr,
        f"FORGED BY {card.source.upper()}  •  CARD OF THE DAY",
        0,
        CARD_H - 26,
        8,
        "#64748b",
        align="center",
    )


def draw_card(cr, card: CardData, art_surface, w: float, h: float):
    """Draw the card, scaled to fit a w x h area while keeping aspect ratio."""
    scale = min(w / CARD_W, h / CARD_H)
    cr.save()
    cr.translate((w - CARD_W * scale) / 2, (h - CARD_H * scale) / 2)
    cr.scale(scale, scale)
    _draw_card_body(cr, card, art_surface)
    cr.restore()


# ---------------------------------------------------------------------------
# GTK application
# ---------------------------------------------------------------------------


class CardApp(Gtk.Application):
    def __init__(self):
        super().__init__(
            application_id="io.qwen.cardoftheday",
            flags=Gio.ApplicationFlags.NON_UNIQUE,
        )
        self.card: CardData | None = None
        self.art_surface = None
        self._today = date.today()
        self._busy = False

    # -- window ----------------------------------------------------------
    def do_activate(self):
        TIMING.mark("gtk_activate")
        win = Gtk.ApplicationWindow(application=self)
        win.set_title("Card of the Day — Qwen3.8-27B")
        win.set_default_size(600, 860)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        win.set_child(root)

        self.area = Gtk.DrawingArea()
        self.area.set_content_width(CARD_W + 20)
        self.area.set_content_height(CARD_H + 20)
        self.area.set_halign(Gtk.Align.CENTER)
        self.area.set_vexpand(True)
        self.area.set_draw_func(self._on_draw)

        bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        bar.set_halign(Gtk.Align.CENTER)
        self.status_label = Gtk.Label(label="Warming up the forge…")
        self.status_label.set_halign(Gtk.Align.CENTER)
        btn_redraw = Gtk.Button(label="Redraw")
        btn_redraw.connect("clicked", self._on_redraw)
        btn_save = Gtk.Button(label="Save PNG")
        btn_save.connect("clicked", self._on_save_png)
        bar.append(self.status_label)
        bar.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))
        bar.append(btn_redraw)
        bar.append(btn_save)

        root.append(self.area)
        root.append(bar)

        win.present()
        TIMING.mark("window_presented")
        self._start_forge()

    # -- drawing ---------------------------------------------------------
    def _on_draw(self, area, cr, width, height):
        if self.card is None:
            cr.set_source_rgb(0.07, 0.06, 0.12)
            cr.paint()
            cr.set_source_rgb(0.7, 0.7, 0.85)
            cr.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
            cr.set_font_size(18)
            msg = "Consulting the Oracle…"
            ext = cr.text_extents(msg)
            cr.move_to((width - ext.width) / 2, height / 2)
            cr.show_text(msg)
            return
        draw_card(cr, self.card, self.art_surface, width, height)
        if not getattr(self, "_first_paint_marked", False):
            TIMING.mark("first_card_paint")
            self._first_paint_marked = True

    # -- forging ---------------------------------------------------------
    def _start_forge(self):
        self._busy = True
        self.status_label.set_text("Consulting the Oracle… (this can take a minute)")
        threading.Thread(target=self._forge_worker, daemon=True).start()

    def _forge_worker(self):
        today = self._today
        try:
            card = forge_card(today)
        except Exception as exc:  # noqa: BLE001
            card = fallback_card(today, reason=str(exc)[:80])
        GLib.idle_add(self._card_ready, card)

    def _card_ready(self, card: CardData) -> bool:
        self.card = card
        self._busy = False
        self.art_surface = None
        if card.svg:
            try:
                self.art_surface = render_svg_to_surface(card.svg, 904, 480)
            except Exception:
                self.art_surface = None
        if card.source.startswith("fallback"):
            self.status_label.set_text(
                f"Oracle unreachable — fallback card ({card.source})"
            )
        else:
            usage = TIMING.token_usage
            tok = (
                f" • {usage.get('total_tokens', '?')} tokens"
                if usage.get("total_tokens") is not None
                else ""
            )
            self.status_label.set_text(
                f"Forged by {card.source} • {card.rarity.upper()} • {card.date_label}{tok}"
            )
        self.area.queue_draw()
        TIMING.mark("card_ready_shown")
        TIMING.write_report(
            TIMING_REPORT_PATH,
            extra={
                "model": MODEL,
                "api_url": API_URL,
                "card": {
                    "name": self.card.name,
                    "rarity": self.card.rarity,
                    "date_label": self.card.date_label,
                    "source": self.card.source,
                },
            },
        )
        return False

    # -- buttons -----------------------------------------------------------
    def _on_redraw(self, _btn):
        if not self._busy:
            self._start_forge()

    def _on_save_png(self, _btn):
        if self.card is None:
            return
        scale = 2
        surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, CARD_W * scale, CARD_H * scale)
        cr = cairo.Context(surf)
        cr.scale(scale, scale)
        draw_card(cr, self.card, self.art_surface, CARD_W, CARD_H)
        out = os.path.join(HERE, f"card-of-the-day-{self._today.strftime('%Y%m%d')}.png")
        surf.write_to_png(out)
        self.status_label.set_text(f"Saved: {out}")


def main():
    app = CardApp()
    return app.run(None)


if __name__ == "__main__":
    sys.exit(main())
