/*
 * card-of-the-day — a GTK4 app that mints a collectible "trading card" for
 * today's date, using a local LLM (Qwen3.8-27B served by vLLM) as the
 * "Oracle of Days".
 *
 * How it works:
 *   1. On startup it detects the current calendar day (or honours a
 *      COTD_DATE=YYYY-MM-DD override for testing).
 *   2. It asks the local LLM (OpenAI-compatible API on 127.0.0.1:8000) to
 *      recall what it knows about that day in history and to mint a
 *      trading-card description: name, epithet, type line, rarity, a power
 *      level, five stats, flavor text, fun facts, and a piece of SVG
 *      artwork illustrating the day's theme.
 *   3. The response is parsed (json-glib), the SVG artwork is rasterised
 *      with librsvg, and the result is laid out as a Pokemon/MtG-style
 *      trading card in a GTK4 window.
 *   4. If the LLM is unreachable or returns garbage, a built-in fallback
 *      card is shown so the app always shows *something*.
 *
 * Build:  make
 * Run:    ./card-of-the-day
 *
 * See README.md for the full story, including the original prompt.
 */

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <librsvg/rsvg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define LLM_HOST "127.0.0.1"
#define LLM_PORT 8000
#define LLM_PATH "/v1/chat/completions"
#define LLM_MODEL "Qwen/Qwen3.8-27B"

#define ART_W 440          /* artwork display width,  px */
#define ART_H 300          /* artwork display height, px */
#define ART_SCALE 2        /* render SVG at 2x for crispness */

/* The five stats every card carries, in display order. The LLM is asked
 * for exactly these keys; anything missing falls back to 50. */
static const char *STAT_NAMES[5] = {
    "Historical Weight",
    "Cultural Resonance",
    "Scientific Impact",
    "Chaos Factor",
    "Vibes",
};

static const char *MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *WEEKDAYS[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};

/* ------------------------------------------------------------------ */
/* Data model                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;        /* e.g. "September 23"                    */
    char *title;       /* epithet, e.g. "The Autumnal Equinox"   */
    char *type_line;   /* e.g. "Celestial Phenomenon / Equinox"  */
    char *rarity;      /* Common | Uncommon | Rare | Epic | Legendary */
    int   power;       /* 1..100 overall power level             */
    double stats[5];   /* one per STAT_NAMES entry, 1..100       */
    char *flavor;      /* one or two evocative sentences         */
    GPtrArray *facts;  /* of char*                               */
    char *svg;         /* SVG artwork markup                     */
    gboolean from_llm; /* FALSE => built-in fallback card        */
} CardData;

typedef struct {
    int  year, month, day;
    int  day_of_year;   /* 1-based */
    int  days_in_year;  /* 365 or 366 */
    char date_str[16];  /* "2026-09-23" */
    char weekday[16];
    char month_name[16];
} DateInfo;

/* ------------------------------------------------------------------ */
/* Globals (single-window app; kept deliberately simple)               */
/* ------------------------------------------------------------------ */

static DateInfo  g_date;        /* today (or COTD_DATE override)   */
static GtkWidget *g_card_slot;  /* box holding card OR loader      */
static gboolean   g_fetching = FALSE;

/* Forward declarations (used before their definitions below). */
static gboolean present_card(gpointer data);
static void     start_fetch(void);

/* Debug aid: if COTD_AUTO_REROLL_SECONDS is set, automatically trigger a
 * re-roll N seconds after a card is presented. Used to reproduce the
 * re-roll crash deterministically. */
static gboolean auto_reroll_cb(gpointer data)
{
    (void) data;
    g_printerr("[debug] auto re-roll triggered\n");
    start_fetch();
    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

/* Append `s` to a GString, JSON-escaped (quotes, backslash, control chars). */
static void json_escape_append(GString *out, const char *s)
{
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '"':  g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n");  break;
        case '\r': g_string_append(out, "\\r");  break;
        case '\t': g_string_append(out, "\\t");  break;
        default:
            if ((unsigned char) *p < 0x20)
                g_string_append_printf(out, "\\u%04x", (unsigned char) *p);
            else
                g_string_append_c(out, *p);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Date handling                                                       */
/* ------------------------------------------------------------------ */

static gboolean is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* Fill g_date from the local clock, or from COTD_DATE=YYYY-MM-DD if set. */
static void compute_date_info(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    const char *env = g_getenv("COTD_DATE");
    if (env && *env) {
        int y, m, d;
        if (sscanf(env, "%d-%d-%d", &y, &m, &d) == 3 &&
            m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            struct tm t = { 0 };
            t.tm_year  = y - 1900;
            t.tm_mon   = m - 1;
            t.tm_mday  = d;
            t.tm_hour  = 12;
            t.tm_isdst = -1;
            time_t t2 = mktime(&t);
            if (t2 != (time_t) -1)
                localtime_r(&t2, &lt);
        }
    }

    g_date.year          = lt.tm_year + 1900;
    g_date.month         = lt.tm_mon + 1;
    g_date.day           = lt.tm_mday;
    g_date.day_of_year   = lt.tm_yday + 1;
    g_date.days_in_year  = is_leap(g_date.year) ? 366 : 365;
    g_snprintf(g_date.date_str, sizeof g_date.date_str,
               "%04d-%02d-%02d", g_date.year, g_date.month, g_date.day);
    g_strlcpy(g_date.weekday, WEEKDAYS[lt.tm_wday], sizeof g_date.weekday);
    g_strlcpy(g_date.month_name, MONTHS[lt.tm_mon], sizeof g_date.month_name);
}

/* ------------------------------------------------------------------ */
/* LLM request / response                                              */
/* ------------------------------------------------------------------ */

/*
 * Build the JSON request body for the chat completions endpoint.
 *
 * Notes learned the hard way (see README.md):
 *  - This model REJECTS response_format (it is a "block-output" model),
 *    so we cannot force JSON mode; the prompt must carry the discipline.
 *  - chat_template_kwargs.enable_thinking=false is essential: without it
 *    the model burns its whole token budget on internal reasoning and
 *    returns an empty `content` field.
 */
static char *build_request_body(void)
{
    GString *sys = g_string_new(
        "You are the Oracle of Days, an ancient oracle that personifies "
        "calendar days. You create collectible trading cards (like Pokemon "
        "or Magic: The Gathering cards) for calendar days. You know deep "
        "history, astronomy, culture, and trivia. You ALWAYS respond with a "
        "single valid JSON object and nothing else - no markdown, no code "
        "fences, no commentary.");

    GString *user = g_string_new(NULL);
    g_string_append_printf(user,
        "Today is %s (%s), day %d of %d in a %s year.\n\n"
        "Create a collectible trading card for TODAY. Return ONLY a JSON "
        "object with EXACTLY these keys:\n"
        "{\n"
        "  \"card_name\": \"short name, e.g. 'September 23'\",\n"
        "  \"title\": \"an evocative epithet for the day, e.g. 'The "
        "Autumnal Equinox'\",\n"
        "  \"card_type\": \"a creature-type line, e.g. 'Celestial "
        "Phenomenon / Equinox'\",\n"
        "  \"rarity\": one of Common, Uncommon, Rare, Epic, Legendary "
        "(judge how special today is),\n"
        "  \"power_level\": integer 1-100, the overall power of this day,\n"
        "  \"stats\": {\"Historical Weight\": int 1-100, "
        "\"Cultural Resonance\": int 1-100, \"Scientific Impact\": int "
        "1-100, \"Chaos Factor\": int 1-100, \"Vibes\": int 1-100},\n"
        "  \"flavor_text\": \"one or two evocative sentences of card "
        "flavor text\",\n"
        "  \"fun_facts\": [3 to 5 short factual statements about this "
        "calendar day: real historical events, observances, holidays, "
        "astronomical events, famous birthdays or deaths. Must be real "
        "facts you actually know.],\n"
        "  \"svg_artwork\": \"a complete standalone SVG string (no "
        "markdown fences) illustrating today's theme. Use viewBox='0 0 400 "
        "300'. Use gradients and simple geometric shapes. No external "
        "references, no scripts, no <text> elements. Make it visually "
        "striking and on-theme.\"\n"
        "}",
        g_date.date_str, g_date.weekday, g_date.day_of_year,
        g_date.days_in_year, is_leap(g_date.year) ? "leap" : "non-leap");

    GString *body = g_string_new("{");
    g_string_append_printf(body, "\"model\":\"%s\",", LLM_MODEL);
    g_string_append(body, "\"messages\":[");
    g_string_append(body, "{\"role\":\"system\",\"content\":\"");
    json_escape_append(body, sys->str);
    g_string_append(body, "\"},{\"role\":\"user\",\"content\":\"");
    json_escape_append(body, user->str);
    g_string_append(body,
        "\"}],\"max_tokens\":8000,\"temperature\":0.7,"
        "\"chat_template_kwargs\":{\"enable_thinking\":false}}");

    g_string_free(sys, TRUE);
    g_string_free(user, TRUE);
    return g_string_free(body, FALSE);
}

/*
 * POST the request body to the LLM using `curl` via g_spawn_sync.
 *
 * We deliberately shell out to curl rather than hand-rolling an HTTP
 * client: it is far more robust across this environment's network stack
 * and keeps the C code simple. The request body is written to a temp
 * file and passed with `-d @file` to avoid any shell-quoting hazards.
 *
 * Returns the response body (caller frees) or NULL on failure.
 */
static char *llm_request(const char *body, GError **error)
{
    /* Fixed, PID-unique path so concurrent runs don't clobber each other. */
    char *body_file = g_strdup_printf("/tmp/cotd_req_%d.json",
                                      (int) getpid());

    if (!g_file_set_contents(body_file, body, -1, NULL)) {
        g_set_error(error, g_quark_from_static_string("llm"), 1,
                    "could not write request body to %s", body_file);
        g_free(body_file);
        return NULL;
    }

    char url[128];
    g_snprintf(url, sizeof url, "http://%s:%d%s",
               LLM_HOST, LLM_PORT, LLM_PATH);

    /* curl reads the request body from the file via "-d @<path>". */
    char *data_arg = g_strdup_printf("@%s", body_file);

    char *argv[] = {
        "curl", "-s", "--max-time", "180",
        "-X", "POST",
        "-H", "Content-Type: application/json",
        "-d", data_arg,
        url,
        NULL
    };

    char *stdout_content = NULL;
    char *stderr_content = NULL;
    int exit_status = 0;
    GError *serr = NULL;

    gboolean ok = g_spawn_sync(
        NULL,            /* working dir: current */
        argv,            /* command + args, NULL-terminated */
        NULL,           /* envp: inherit */
        G_SPAWN_SEARCH_PATH,
        NULL,           /* child setup */
        NULL,           /* child env */
        &stdout_content,
        &stderr_content,
        &exit_status,
        &serr);

    g_free(data_arg);
    unlink(body_file);
    g_free(body_file);

    if (!ok) {
        if (serr) {
            g_set_error(error, g_quark_from_static_string("llm"), 2,
                        "spawn failed: %s", serr->message);
            g_error_free(serr);
        } else {
            g_set_error(error, g_quark_from_static_string("llm"), 3,
                        "curl exited with status %d", exit_status);
        }
        g_free(stderr_content);
        return NULL;
    }

    if (exit_status != 0) {
        g_set_error(error, g_quark_from_static_string("llm"), 4,
                    "curl exited with status %d: %s",
                    exit_status,
                    stderr_content ? stderr_content : "(no stderr)");
        g_free(stderr_content);
        g_free(stdout_content);
        return NULL;
    }

    g_free(stderr_content);
    return stdout_content; /* caller frees */
}

/* Parse the card JSON out of the model's text content. The model is
 * instructed to return bare JSON, but we defensively locate the
 * outermost { ... } pair before parsing. */
static gboolean parse_card_json(const char *content, CardData *card,
                                GError **error)
{
    const char *start = strchr(content, '{');
    const char *end = strrchr(content, '}');
    if (!start || !end || end <= start) {
        g_set_error(error, g_quark_from_static_string("parse"), 1,
                    "no JSON object found in model output");
        return FALSE;
    }
    size_t len = (size_t)(end - start) + 1;
    char *snippet = g_strndup(start, len);

    JsonParser *parser = json_parser_new();
    GError *jerr = NULL;
    gboolean ok = json_parser_load_from_data(parser, snippet, -1, &jerr);
    if (ok) {
        JsonNode *root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
            g_set_error(error, g_quark_from_static_string("parse"), 2,
                        "root JSON is not an object");
            ok = FALSE;
        }
    }
    if (!ok) {
        if (jerr) g_error_free(jerr);
        g_object_unref(parser);
        g_free(snippet);
        if (!*error)
            g_set_error(error, g_quark_from_static_string("parse"), 3,
                        "invalid JSON from model");
        return FALSE;
    }

    JsonObject *root = json_node_get_object(json_parser_get_root(parser));

    const char *s;
    if ((s = json_node_get_string(
             json_object_get_member(root, "card_name"))))
        card->name = g_strdup(s);
    if ((s = json_node_get_string(
             json_object_get_member(root, "title"))))
        card->title = g_strdup(s);
    if ((s = json_node_get_string(
             json_object_get_member(root, "card_type"))))
        card->type_line = g_strdup(s);
    if ((s = json_node_get_string(
             json_object_get_member(root, "rarity"))))
        card->rarity = g_strdup(s);
    if ((s = json_node_get_string(
             json_object_get_member(root, "flavor_text"))))
        card->flavor = g_strdup(s);
    if ((s = json_node_get_string(
             json_object_get_member(root, "svg_artwork"))))
        card->svg = g_strdup(s);

    JsonNode *pn = json_object_get_member(root, "power_level");
    if (pn && json_node_get_node_type(pn) == JSON_NODE_VALUE)
        card->power = CLAMP(json_node_get_int(pn), 1, 100);

    JsonNode *stats_node = json_object_get_member(root, "stats");
    JsonObject *stats = (stats_node && JSON_NODE_HOLDS_OBJECT(stats_node))
                         ? json_node_get_object(stats_node) : NULL;
    for (int i = 0; i < 5; i++) {
        double v = 50.0; /* neutral default */
        if (stats) {
            JsonNode *sn = json_object_get_member(stats, STAT_NAMES[i]);
            if (sn && json_node_get_node_type(sn) == JSON_NODE_VALUE)
                v = CLAMP(json_node_get_double(sn), 1.0, 100.0);
        }
        card->stats[i] = v;
    }

    JsonNode *facts_node = json_object_get_member(root, "fun_facts");
    card->facts = g_ptr_array_new();
    if (facts_node && JSON_NODE_HOLDS_ARRAY(facts_node)) {
        JsonArray *arr = json_node_get_array(facts_node);
        guint n = json_array_get_length(arr);
        for (guint i = 0; i < n; i++) {
            JsonNode *el = json_array_get_element(arr, i);
            if (json_node_get_node_type(el) == JSON_NODE_VALUE) {
                const char *f = json_node_get_string(el);
                if (f && *f)
                    g_ptr_array_add(card->facts, g_strdup(f));
            }
        }
    }

    g_object_unref(parser);
    g_free(snippet);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Card construction                                                   */
/* ------------------------------------------------------------------ */

/* Built-in fallback card, used when the LLM is unreachable or returns
 * unusable output. The app must always show *something*. */
static CardData *make_fallback_card(void)
{
    CardData *c = g_new0(CardData, 1);
    c->name = g_strdup_printf("%s %d", g_date.month_name, g_date.day);
    c->title = g_strdup("The Uncharted Day");
    c->type_line = g_strdup("Mystery / Uncharted");
    c->rarity = g_strdup("Common");
    c->power = 50;
    for (int i = 0; i < 5; i++)
        c->stats[i] = 50.0;
    c->flavor = g_strdup("The Oracle is unreachable. This card was forged "
                         "in the void between servers.");
    c->facts = g_ptr_array_new();
    g_ptr_array_add(c->facts,
        g_strdup("The Oracle (Qwen3.8-27B) could not be reached."));
    g_ptr_array_add(c->facts,
        g_strdup("This card was generated from the built-in fallback "
                 "template."));
    g_ptr_array_add(c->facts,
        g_strdup("Check the LLM server and press Re-roll to try again."));
    c->svg = g_strdup_printf(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 400 300'>"
        "<defs><linearGradient id='bg' x1='0' y1='0' x2='0' y2='1'>"
        "<stop offset='0%%' stop-color='#0e2a2b'/>"
        "<stop offset='100%%' stop-color='#071a19'/>"
        "</linearGradient></defs>"
        "<rect width='400' height='300' fill='url(#bg)'/>"
        "<circle cx='200' cy='128' r='72' fill='none' "
        "stroke='#1B8EB1' stroke-width='3'/>"
        "<circle cx='200' cy='128' r='46' fill='#1B8EB1' opacity='0.55'/>"
        "<text x='200' y='145' font-family='sans-serif' font-size='44' "
        "font-weight='bold' fill='#f1f8f8' text-anchor='middle'>%d</text>"
        "<text x='200' y='238' font-family='sans-serif' font-size='20' "
        "fill='#74C5DF' text-anchor='middle'>%s %d</text>"
        "<text x='200' y='262' font-family='sans-serif' font-size='12' "
        "fill='#5f7d7b' text-anchor='middle'>The Oracle is "
        "unreachable</text>"
        "</svg>",
        g_date.day, g_date.month_name, g_date.day);
    c->from_llm = FALSE;
    return c;
}

/* Free a CardData and everything it owns. */
static void card_data_free(CardData *c)
{
    if (!c)
        return;
    g_free(c->name);
    g_free(c->title);
    g_free(c->type_line);
    g_free(c->rarity);
    g_free(c->flavor);
    g_free(c->svg);
    if (c->facts) {
        for (guint i = 0; i < c->facts->len; i++)
            g_free(g_ptr_array_index(c->facts, i));
        g_ptr_array_free(c->facts, TRUE);
    }
    g_free(c);
}

/* ------------------------------------------------------------------ */
/* LLM fetch (runs in a background GThread)                            */
/* ------------------------------------------------------------------ */

static void *fetch_thread(gpointer data)
{
    (void) data;
    CardData *card = NULL;
    GError *err = NULL;

    char *body = build_request_body();
    char *response = llm_request(body, &err);
    g_free(body);

    if (response) {
        /* The response is the OpenAI-style chat completion JSON. Pull
         * out choices[0].message.content, then parse the card JSON out
         * of that text. */
        JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, response, -1, NULL)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                JsonObject *root_obj = json_node_get_object(root);
                JsonNode *choices =
                    json_object_get_member(root_obj, "choices");
                if (choices && JSON_NODE_HOLDS_ARRAY(choices)) {
                    JsonArray *arr = json_node_get_array(choices);
                    if (json_array_get_length(arr) > 0) {
                        JsonNode *choice =
                            json_array_get_element(arr, 0);
                        JsonNode *msg = json_object_get_member(
                            json_node_get_object(choice), "message");
                        if (msg) {
                            JsonNode *content_node =
                                json_object_get_member(
                                    json_node_get_object(msg),
                                    "content");
                            const char *content =
                                content_node ?
                                json_node_get_string(content_node)
                                             : NULL;
                            if (content && *content) {
                                CardData *c = g_new0(CardData, 1);
                                c->from_llm = TRUE;
                                if (parse_card_json(content, c, &err)) {
                                    card = c;
                                } else {
                                    g_warning(
                                        "card JSON parse failed: %s",
                                        err ? err->message : "?");
                                    g_clear_error(&err);
                                    card_data_free(c);
                                }
                            }
                        }
                    }
                }
            }
            g_object_unref(parser);
        }
        g_free(response);
    } else if (err) {
        g_warning("LLM request failed: %s", err->message);
        g_clear_error(&err);
    }

    if (!card)
        card = make_fallback_card();

    /* Hand off to the main thread. */
    g_idle_add(present_card, card);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* UI construction                                                     */
/* ------------------------------------------------------------------ */

/* Render an SVG string to a GdkTexture at ART_W x ART_H (2x supersampled).
 * Returns NULL if the SVG cannot be parsed. */
static GdkTexture *svg_to_texture(const char *svg)
{
    GError *err = NULL;
    /* NOTE: this librsvg build requires an explicit data length; it does
     * not accept -1 to mean "NUL-terminated". */
    RsvgHandle *h = rsvg_handle_new_from_data(
        (const guint8 *) svg, (gsize) strlen(svg), &err);
    if (!h) {
        g_warning("SVG parse failed: %s", err ? err->message : "?");
        g_clear_error(&err);
        return NULL;
    }

    double iw = 0, ih = 0;
    if (!rsvg_handle_get_intrinsic_size_in_pixels(h, &iw, &ih) ||
        iw <= 0 || ih <= 0) {
        iw = 400;
        ih = 300;
    }

    int W = ART_W * ART_SCALE;
    int H = ART_H * ART_SCALE;
    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(surf);

    /* Fit the artwork into the target box, preserving aspect ratio. */
    double scale = fmin((double) W / iw, (double) H / ih);
    double ox = (W - iw * scale) / 2.0;
    double oy = (H - ih * scale) / 2.0;
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);

    RsvgRectangle viewport = { 0, 0, iw, ih };
    rsvg_handle_render_document(h, cr, &viewport, NULL);

    cairo_destroy(cr);
    GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surf, 0, 0, W, H);
    cairo_surface_destroy(surf);
    g_object_unref(h);

    GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
    g_object_unref(pb);
    return tex;
}

static const char *rarity_css_class(const char *rarity)
{
    if (!rarity)
        return "rarity-common";
    if (g_ascii_strcasecmp(rarity, "Uncommon") == 0)
        return "rarity-uncommon";
    if (g_ascii_strcasecmp(rarity, "Rare") == 0)
        return "rarity-rare";
    if (g_ascii_strcasecmp(rarity, "Epic") == 0)
        return "rarity-epic";
    if (g_ascii_strcasecmp(rarity, "Legendary") == 0)
        return "rarity-legendary";
    return "rarity-common";
}

/* Build one stat row: [name label] [LevelBar] [value label]. */
static GtkWidget *build_stat_row(const char *name, double value)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *nm = gtk_label_new(name);
    gtk_widget_add_css_class(nm, "stat-name");
    gtk_widget_set_size_request(nm, 150, -1);
    gtk_widget_set_halign(nm, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), nm);

    GtkWidget *bar = gtk_level_bar_new_for_interval(0, 100);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), value);
    gtk_widget_set_hexpand(bar, TRUE);
    gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(bar, "stat-bar");
    gtk_box_append(GTK_BOX(row), bar);

    char vbuf[16];
    g_snprintf(vbuf, sizeof vbuf, "%.0f", value);
    GtkWidget *vl = gtk_label_new(vbuf);
    gtk_widget_add_css_class(vl, "stat-value");
    gtk_widget_set_size_request(vl, 30, -1);
    gtk_widget_set_halign(vl, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), vl);

    return row;
}

/* Build the full card widget tree from a CardData. */
static GtkWidget *build_card_ui(CardData *card)
{
    GtkWidget *card_w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(card_w, "card");

    /* --- header: name + rarity ------------------------------------- */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name_lbl = gtk_label_new(card->name ? card->name : "?");
    gtk_widget_add_css_class(name_lbl, "card-name");
    gtk_widget_set_hexpand(name_lbl, TRUE);
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hdr), name_lbl);

    GtkWidget *rar_lbl =
        gtk_label_new(card->rarity ? card->rarity : "Common");
    gtk_widget_add_css_class(rar_lbl, rarity_css_class(card->rarity));
    gtk_widget_add_css_class(rar_lbl, "rarity-badge");
    gtk_box_append(GTK_BOX(hdr), rar_lbl);
    gtk_box_append(GTK_BOX(card_w), hdr);

    /* --- artwork ---------------------------------------------------- */
    GtkWidget *art;
    GdkTexture *tex = card->svg ? svg_to_texture(card->svg) : NULL;
    if (tex) {
        art = gtk_image_new_from_paintable(GDK_PAINTABLE(tex));
        g_object_unref(tex);
    } else {
        art = gtk_image_new_from_icon_name("image-missing");
    }
    gtk_widget_set_size_request(art, ART_W, ART_H);
    gtk_widget_add_css_class(art, "artwork");
    gtk_widget_set_halign(art, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card_w), art);

    /* --- type line -------------------------------------------------- */
    if (card->type_line) {
        GtkWidget *type_lbl = gtk_label_new(card->type_line);
        gtk_widget_add_css_class(type_lbl, "type-line");
        gtk_widget_set_halign(type_lbl, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(card_w), type_lbl);
    }

    /* --- power row --------------------------------------------------- */
    GtkWidget *prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(prow, GTK_ALIGN_END);
    GtkWidget *p_lbl = gtk_label_new("POWER");
    gtk_widget_add_css_class(p_lbl, "power-label");
    GtkWidget *p_val = gtk_label_new(NULL);
    char pbuf[16];
    g_snprintf(pbuf, sizeof pbuf, "%d", card->power);
    gtk_label_set_text(GTK_LABEL(p_val), pbuf);
    gtk_widget_add_css_class(p_val, "power-value");
    gtk_box_append(GTK_BOX(prow), p_lbl);
    gtk_box_append(GTK_BOX(prow), p_val);
    gtk_box_append(GTK_BOX(card_w), prow);

    /* --- stats ------------------------------------------------------- */
    for (int i = 0; i < 5; i++) {
        gtk_box_append(GTK_BOX(card_w),
                       build_stat_row(STAT_NAMES[i], card->stats[i]));
    }

    /* --- flavor text ------------------------------------------------- */
    if (card->flavor) {
        GtkWidget *flav = gtk_label_new(card->flavor);
        gtk_widget_add_css_class(flav, "flavor");
        gtk_label_set_wrap(GTK_LABEL(flav), TRUE);
        gtk_label_set_xalign(GTK_LABEL(flav), 0.5);
        gtk_widget_set_halign(flav, GTK_ALIGN_FILL);
        gtk_box_append(GTK_BOX(card_w), flav);
    }

    /* --- fun facts --------------------------------------------------- */
    if (card->facts && card->facts->len > 0) {
        GtkWidget *sec = gtk_label_new("FUN FACTS");
        gtk_widget_add_css_class(sec, "section-label");
        gtk_widget_set_halign(sec, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(card_w), sec);

        for (guint i = 0; i < card->facts->len; i++) {
            const char *f = g_ptr_array_index(card->facts, i);
            GtkWidget *fl = gtk_label_new(NULL);
            char *prefixed = g_strdup_printf("\u2022  %s", f);
            gtk_label_set_text(GTK_LABEL(fl), prefixed);
            g_free(prefixed);
            gtk_widget_add_css_class(fl, "fact");
            gtk_label_set_wrap(GTK_LABEL(fl), TRUE);
            gtk_label_set_xalign(GTK_LABEL(fl), 0.0);
            gtk_widget_set_halign(fl, GTK_ALIGN_FILL);
            gtk_box_append(GTK_BOX(card_w), fl);
        }
    }

    /* --- footer ------------------------------------------------------ */
    char fbuf[128];
    g_snprintf(fbuf, sizeof fbuf, "COTD  %d/%d   \u2022   Qwen3.8-27B",
               g_date.day_of_year, g_date.days_in_year);
    GtkWidget *foot = gtk_label_new(fbuf);
    gtk_widget_add_css_class(foot, "footer");
    gtk_widget_set_halign(foot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(card_w), foot);

    return card_w;
}

/* ------------------------------------------------------------------ */
/* Presentation / lifecycle                                            */
/* ------------------------------------------------------------------ */

/* Main-thread callback: swap the card into the window. */
static gboolean present_card(gpointer data)
{
    CardData *card = data;
    GtkWidget *card_w = build_card_ui(card);
    card_data_free(card); /* UI now owns all needed values */

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(g_card_slot)))
        gtk_box_remove(GTK_BOX(g_card_slot), child);
    gtk_box_append(GTK_BOX(g_card_slot), card_w);
    g_fetching = FALSE;

    /* Debug aid: auto re-roll after N seconds if requested via env var. */
    const char *rr = g_getenv("COTD_AUTO_REROLL_SECONDS");
    if (rr && *rr) {
        int secs = atoi(rr);
        if (secs > 0)
            g_timeout_add_seconds(secs, auto_reroll_cb, NULL);
    }
    return G_SOURCE_REMOVE;
}

/* Build a fresh "consulting the oracle" loading widget.
 *
 * A NEW widget is created on every call (rather than re-using a persistent
 * one) because this GTK build destroys a widget when it is removed from its
 * container: the container is the widget's sole owner, so re-parenting a
 * previously-removed widget would use a dangling pointer. Building a fresh
 * loader each time sidesteps that entirely. */
static GtkWidget *make_loading_widget(void)
{
    GtkWidget *loading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);

    GtkWidget *lbl = gtk_label_new("Consulting the Oracle\u2026\n"
                                    "(this can take a minute on TT hardware)");
    gtk_widget_add_css_class(lbl, "loading-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(loading), spinner);
    gtk_box_append(GTK_BOX(loading), lbl);
    gtk_widget_set_halign(loading, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(loading, TRUE);
    return loading;
}

static void start_fetch(void)
{
    if (g_fetching)
        return;
    g_fetching = TRUE;

    /* Clear the slot and show a fresh loader. */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(g_card_slot)))
        gtk_box_remove(GTK_BOX(g_card_slot), child);
    gtk_box_append(GTK_BOX(g_card_slot), make_loading_widget());

    GThread *t = g_thread_new("cotd-fetch", fetch_thread, NULL);
    (void) t; /* thread detaches; result arrives via g_idle_add */
}

static void on_reroll_clicked(GtkButton *btn, gpointer data)
{
    (void) btn;
    (void) data;
    start_fetch();
}

/* ------------------------------------------------------------------ */
/* CSS                                                                 */
/* ------------------------------------------------------------------ */

static const char *APP_CSS =
    "window { background: #0a1214; }"
    ".card {"
    "  background: linear-gradient(160deg, #10262a 0%, #092221 60%, "
    "#071a19 100%);"
    "  border: 3px solid #1B8EB1;"
    "  border-radius: 18px;"
    "  padding: 14px;"
    "}"
    ".card-name { font-size: 22px; font-weight: 800; color: #f1f8f8; }"
    ".rarity-badge { font-size: 12px; font-weight: 700; padding: 2px 8px; "
    "border-radius: 8px; background: rgba(255,255,255,0.06); }"
    ".rarity-common { color: #9ca3af; }"
    ".rarity-uncommon { color: #4ade80; }"
    ".rarity-rare { color: #60a5fa; }"
    ".rarity-epic { color: #c084fc; }"
    ".rarity-legendary { color: #fbbf24; }"
    ".artwork { border-radius: 8px; }"
    ".type-line { font-size: 11px; color: #8fb8b5; letter-spacing: 1.5px; }"
    ".power-label { font-size: 11px; color: #74C5DF; letter-spacing: 2px; "
    "font-weight: 700; }"
    ".power-value { font-size: 26px; font-weight: 800; color: #f1f8f8; }"
    ".stat-name { font-size: 11px; color: #9fc9c5; }"
    ".stat-value { font-size: 11px; font-weight: 700; color: #f1f8f8; }"
    ".stat-bar trough { min-height: 10px; border-radius: 5px; "
    "background-color: #10302e; }"
    ".stat-bar block.filled { background-color: #1B8EB1; "
    "border-radius: 5px; }"
    ".flavor { font-style: italic; color: #cfe8e6; font-size: 12.5px; "
    "padding: 4px 6px; }"
    ".section-label { font-size: 10px; font-weight: 700; color: #1B8EB1; "
    "letter-spacing: 2px; margin-top: 4px; }"
    ".fact { font-size: 11.5px; color: #bfe3e0; }"
    ".footer { font-size: 9px; color: #5f7d7b; margin-top: 4px; }"
    ".loading-label { color: #74C5DF; font-size: 14px; }"
    "button { background: #10302f; color: #74C5DF; border-radius: 10px; "
    "padding: 8px 18px; font-weight: 700; border: 1px solid #1B8EB1; }"
    "button:hover { background: #16403e; }";

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    gtk_init();
    compute_date_info();

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, APP_CSS, -1);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* Window */
    GtkWidget *win = gtk_window_new();
    char title[128];
    g_snprintf(title, sizeof title, "Card of the Day \u2014 %s %d, %d",
               g_date.month_name, g_date.day, g_date.year);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 900);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    /* header */
    GtkWidget *hdr_lbl = gtk_label_new(NULL);
    char hdr[128];
    g_snprintf(hdr, sizeof hdr, "CARD OF THE DAY  \u2022  %s %d, %d",
               g_date.month_name, g_date.day, g_date.year);
    gtk_label_set_text(GTK_LABEL(hdr_lbl), hdr);
    gtk_widget_add_css_class(hdr_lbl, "section-label");
    gtk_widget_set_halign(hdr_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(root), hdr_lbl);

    /* card slot (start_fetch() populates it with a fresh loader) */
    g_card_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), g_card_slot);

    /* re-roll button */
    GtkWidget *reroll = gtk_button_new_with_label(
        "\u27F3  Re-roll the Oracle");
    gtk_widget_set_halign(reroll, GTK_ALIGN_CENTER);
    g_signal_connect(reroll, "clicked", G_CALLBACK(on_reroll_clicked), NULL);
    gtk_box_append(GTK_BOX(root), reroll);

    gtk_window_set_child(GTK_WINDOW(win), root);

    /* kick off the first fetch */
    start_fetch();

    gtk_window_present(GTK_WINDOW(win));

    /* This GTK build has no gtk_run(); drive the default main context
     * with an explicit GMainLoop instead. */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return 0;
}
