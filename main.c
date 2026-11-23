/*
 * Clicking Bad - native SDL2 version.
 * Game rules and data follow the original web game by Aaron Meier and contributors
 * (https://github.com/nullism/clickingbad, MIT). This port: MIT as well.
 */
#include <SDL.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"
#include "stb_truetype.h"

/* ---------------------------------------------------------------- data */

enum { CAT_NONE = -1, CAT_CLICKERS, CAT_SELLERS, CAT_BANKS, NCAT };
enum { A_NONE, A_MAKE_AMOUNT, A_MAKE_MULT, A_SELL_AMOUNT, A_SELL_MULT, A_WIDGET_ROI, A_ITEM_RPS, A_ITEM_RISK };
enum { P_STAT, P_AMOUNT };
enum { EV_FOUND_CASH, EV_FOUND_METH, EV_DEA_SEIZE_BUILDING, EV_LOSE_CASH, EV_RIVAL_LOSE_CASH,
       EV_PAY_BRIBE, EV_LOSE_METH, EV_IRS_AUDIT };

typedef struct { const char *key, *sid, *label, *desc; double rps, risk, base_cost, unlock_rps; int unlocked; } ItemDef;
typedef struct { const char *key, *sid, *label, *desc; double cost, mod; int action, cat, idx, prereq, purchased; } UpgradeDef;
typedef struct { const char *key, *sid, *label, *desc; int kind, cat, idx; double required; int hidden; } AchDef;
typedef struct { double level; const char *label; } Quality;
typedef struct { double level; const char *label; } RiskLevel;
typedef struct { double chance; int type; double arg; } EventDef;

#include "data.h"

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define NUPG LEN(upgrades_def)
#define NACH LEN(achievements_def)
#define MAXITEMS 32

/* The first six are the ones achievements look at (same order as gen_data.js). */
enum { ST_HAND_MADE, ST_TOTAL_CASH, ST_TOTAL_SPENT, ST_SECONDS, ST_CHEATED_CASH, ST_CHEATED_WIDGETS,
       ST_MADE, ST_SOLD, ST_HAND_SOLD, ST_BOUGHT_UPG, ST_SELLER_RPS, ST_CLICKER_RPS, ST_BANK_RPS, NSTAT };
static const char *stat_names[NSTAT] = {
    "hand_made_widgets", "total_cash", "total_spent", "seconds_played", "cheated_cash", "cheated_widgets",
    "made_widgets", "sold_widgets", "hand_sold_widgets", "bought_upgrades", "seller_rps", "clicker_rps", "bank_rps",
};

static const ItemDef *cat_def[NCAT] = { clickers_def, sellers_def, banks_def };
static const int cat_n[NCAT] = { LEN(clickers_def), LEN(sellers_def), LEN(banks_def) };

typedef struct { double amount, rps, risk; int unlocked; } Item;

static struct {
    double cash, safe, widgets;
    double make_amount, make_mult, sell_amount, sell_mult, sell_return, economy_roi, widget_roi;
    double risk_amount, risk2_amount;
    Item item[NCAT][MAXITEMS];
    int upg[NUPG > 0 ? NUPG : 1];
    int ach[NACH > 0 ? NACH : 1];
    double stat[NSTAT];
} G;

/* ---------------------------------------------------------------- messages */

enum { MSG_NORMAL, MSG_GOOD, MSG_BAD, MSG_ERROR };
#define NMSG 45
typedef struct { char text[256]; int type; } Msg;
static Msg msgs[NMSG];
static int nmsgs;

static void add_message(int type, const char *fmt, ...)
{
    static const char *prefix[] = { "\xE2\x9C\x89 ", "\xE2\x98\x85 ", "\xE2\x9C\x97 ", "\xE2\x9C\x97 " };
    va_list ap;
    memmove(&msgs[1], &msgs[0], sizeof(Msg) * (NMSG - 1));
    strcpy(msgs[0].text, prefix[type]);
    va_start(ap, fmt);
    vsnprintf(msgs[0].text + strlen(msgs[0].text), sizeof(msgs[0].text) - 8, fmt, ap);
    va_end(ap);
    msgs[0].type = type;
    if (nmsgs < NMSG) nmsgs++;
}

/* ---------------------------------------------------------------- number formatting (pretty_int / pretty_bigint) */

static double js_round(double x) { return floor(x + 0.5); }

static void commas(char *out, const char *digits)
{
    const char *p = digits;
    int neg = (*p == '-'), n, i;
    if (neg) *out++ = *p++;
    n = (int)strlen(p);
    for (i = 0; i < n; i++) {
        *out++ = p[i];
        if (i < n - 1 && (n - 1 - i) % 3 == 0) *out++ = ',';
    }
    *out = 0;
}

static const char *pretty_int(double num)
{
    static char ring[8][64];
    static int r;
    char *out = ring[r++ & 7], tmp[64];
    if (num < 1000) num = js_round(num * 10) / 10;
    else num = js_round(num);
    if (num == 0) num = 0; /* no -0 */
    if (fabs(num) >= 1e21) { snprintf(out, 64, "%g", num); return out; }
    if (num == floor(num)) {
        snprintf(tmp, sizeof tmp, "%.0f", num);
        commas(out, tmp);
    } else {
        snprintf(out, 64, "%.1f", num);
    }
    return out;
}

static const char *pretty_bigint(double num)
{
    static const struct { double div; const char *suf; } tiers[] = {
        { 1e21, "S" }, { 1e18, "Qt" }, { 1e15, "Q" }, { 1e12, "T" }, { 1e9, "B" }, { 1e6, "M" },
    };
    static char ring[8][64];
    static int r;
    char *out;
    int i;
    if (num >= 1e24) return pretty_int(num);
    for (i = 0; i < LEN(tiers); i++) {
        if (num >= tiers[i].div) {
            char *dot;
            int len;
            out = ring[r++ & 7];
            snprintf(out, 64, "%.2f", js_round((num / tiers[i].div) * 100) / 100);
            dot = strchr(out, '.');
            if (dot) {
                len = (int)strlen(out);
                while (len > 0 && out[len - 1] == '0') out[--len] = 0;
                if (out[len - 1] == '.') out[--len] = 0;
            }
            strcat(out, tiers[i].suf);
            return out;
        }
    }
    return pretty_int(num);
}

/* ---------------------------------------------------------------- game logic (mirrors game.js) */

static Uint64 rng_state = 88172645463325252ull;
static double rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state >> 11) / 9007199254740992.0;
}

static Uint32 last_bust;     /* ms, DEA raid cooldown */
static int bust_ever;

static void reset_game(int keep_achievements)
{
    int c, i;
    int ach[NACH > 0 ? NACH : 1];
    memcpy(ach, G.ach, sizeof ach);
    memset(&G, 0, sizeof G);
    G.make_amount = 1;
    G.sell_amount = 1;
    G.sell_return = START_SELL_RETURN;
    G.economy_roi = 1;
    G.widget_roi = START_WIDGET_ROI;
    for (c = 0; c < NCAT; c++)
        for (i = 0; i < cat_n[c]; i++) {
            G.item[c][i].rps = cat_def[c][i].rps;
            G.item[c][i].risk = cat_def[c][i].risk;
            G.item[c][i].unlocked = cat_def[c][i].unlocked;
        }
    for (i = 0; i < NUPG; i++) G.upg[i] = upgrades_def[i].purchased;
    if (keep_achievements) memcpy(G.ach, ach, sizeof ach);
}

static void earn_cash(double n) { G.cash += n; }

static int spend_cash(double n)
{
    if (n > G.cash) return 0;
    G.cash -= n;
    G.stat[ST_TOTAL_SPENT] += n;
    return 1;
}

static double get_risk(void)
{
    double r = 0;
    int c, i;
    for (c = CAT_CLICKERS; c <= CAT_SELLERS; c++)
        for (i = 0; i < cat_n[c]; i++)
            if (G.item[c][i].risk) r += G.item[c][i].risk * G.item[c][i].amount;
    return r;
}

static double get_risk2(void)
{
    if (G.cash <= 20000) return 0;
    if (G.safe > G.cash) return 0;
    return 0.5 - (G.safe / G.cash);
}

static double click_sell_amount(void) { return G.sell_amount + G.stat[ST_SELLER_RPS] * G.sell_mult; }
static double click_make_amount(void) { return G.make_amount + G.stat[ST_CLICKER_RPS] * G.make_mult; }

static double item_cost(int c, int i)
{
    double a = G.item[c][i].amount + 1;
    double cst = (a * cat_def[c][i].base_cost) * a;
    if (a > 10) cst *= 2;
    return cst;
}

static double item_sell_value(int c, int i)
{
    double a = G.item[c][i].amount;
    double cst = (a * cat_def[c][i].base_cost) * a;
    if (a > 10) cst *= 2;
    return cst * (G.sell_return * G.economy_roi);
}

static double safe_cash(void) { return G.safe > G.cash ? G.cash : G.safe; }

static void do_make(double n)
{
    G.widgets += n;
    G.stat[ST_MADE] += n;
}

static double do_sell(double n)
{
    if (G.widgets < 1) return 0;
    if (n > G.widgets) {
        n = G.widgets;
        if (n < 1) return 0;
    }
    G.stat[ST_SOLD] += n;
    G.widgets -= n;
    earn_cash(n * G.widget_roi);
    G.stat[ST_TOTAL_CASH] += n * G.widget_roi;
    return n;
}

static const char *widget_quality(void)
{
    int i;
    for (i = 0; i < LEN(qualities); i++)
        if (!(G.widget_roi > qualities[i].level)) return qualities[i].label;
    return "NA";
}

static int apply_upgrade(int u)
{
    const UpgradeDef *d = &upgrades_def[u];
    if (G.upg[u]) return 0;
    G.upg[u] = 1;
    switch (d->action) {
    case A_MAKE_AMOUNT: G.make_amount += d->mod; break;
    case A_MAKE_MULT: G.make_mult += d->mod; break;
    case A_SELL_AMOUNT: G.sell_amount += d->mod; break;
    case A_SELL_MULT: G.sell_mult += d->mod; break;
    case A_WIDGET_ROI: G.widget_roi += d->mod; break;
    case A_ITEM_RPS: G.item[d->cat][d->idx].rps += d->mod; break;
    case A_ITEM_RISK: G.item[d->cat][d->idx].risk += d->mod; break;
    }
    return 1;
}

static int upgrade_visible(int u)
{
    const UpgradeDef *d = &upgrades_def[u];
    if (d->prereq >= 0 && !G.upg[d->prereq]) return 0;
    if (d->prereq == -2 && !G.upg[u]) return 0;
    return 1;
}

static void fix_unlocks(void)
{
    int c, i;
    for (c = 0; c < NCAT; c++)
        for (i = 0; i < cat_n[c]; i++)
            if (cat_def[c][i].unlock_rps <= G.stat[ST_SELLER_RPS]) G.item[c][i].unlocked = 1;
}

static void tick(int ticks)
{
    const double sub = 10; /* 1000 / tick_ms */
    double make = 0, sell = 0, safe = 0;
    int i;
    if (G.cash < 0) G.cash = 0;
    if (G.widgets < 0) G.widgets = 0;
    if (G.safe < 0) G.safe = 0;

    for (i = 0; i < cat_n[CAT_CLICKERS]; i++) make += G.item[CAT_CLICKERS][i].amount * G.item[CAT_CLICKERS][i].rps;
    G.stat[ST_CLICKER_RPS] = make;
    do_make(make / sub * ticks);

    for (i = 0; i < cat_n[CAT_SELLERS]; i++) sell += G.item[CAT_SELLERS][i].amount * G.item[CAT_SELLERS][i].rps;
    G.stat[ST_SELLER_RPS] = sell;
    do_sell(sell / sub * ticks);

    for (i = 0; i < cat_n[CAT_BANKS]; i++) safe += G.item[CAT_BANKS][i].amount * G.item[CAT_BANKS][i].rps;
    G.stat[ST_BANK_RPS] = safe;
    G.safe += safe / sub * ticks;
    if (G.safe > G.cash) G.safe = G.cash;

    fix_unlocks();
    G.risk_amount = get_risk();
    G.risk2_amount = get_risk2();
}

static void check_achievements(void)
{
    int a;
    for (a = 0; a < NACH; a++) {
        const AchDef *d = &achievements_def[a];
        double val;
        if (G.ach[a]) continue;
        val = d->kind == P_STAT ? G.stat[d->idx] : G.item[d->cat][d->idx].amount;
        if (val && ((d->required >= 0 && val >= d->required) || (d->required < 0 && val <= d->required))) {
            G.ach[a] = 1;
            add_message(MSG_GOOD, "You have earned a new achievement: %s", d->label);
        }
    }
}

static int saved_ago;

static void sec_tick(void)
{
    int u;
    saved_ago++;
    G.stat[ST_SECONDS] += 1;
    G.stat[ST_BOUGHT_UPG] = 0;
    for (u = 0; u < NUPG; u++) G.stat[ST_BOUGHT_UPG] += G.upg[u];
    check_achievements();
}

/* Events ---------------------------------------------------------------- */

static void event_dea_seize_cash(double n)
{
    double amt = G.cash * n - safe_cash();
    if (amt < 1) { add_message(MSG_GOOD, "The DEA was unable to seize any cash"); return; }
    G.cash -= amt;
    add_message(MSG_BAD, "The DEA has seized $%s!", pretty_bigint(amt));
}

static double cash_loss(double r)
{
    double amt = (G.stat[ST_SELLER_RPS] * G.widget_roi) * r - safe_cash();
    if (amt < 1) return 0;
    if (amt > G.cash) amt = G.cash;
    G.cash -= amt;
    return amt;
}

static void run_event(const EventDef *e, Uint32 now)
{
    double amt, r = e->arg;
    int c, i, picks[MAXITEMS], np = 0;
    switch (e->type) {
    case EV_IRS_AUDIT:
        if (get_risk2() < rnd()) { add_message(MSG_GOOD, "You were able to avoid an IRS audit"); return; }
        amt = G.cash * r - safe_cash();
        if (amt < 1) { add_message(MSG_GOOD, "The IRS was unable to find any unlaundered cash to seize"); return; }
        G.cash -= amt;
        add_message(MSG_BAD, "The IRS has confiscated $%s of your funds", pretty_bigint(amt));
        break;
    case EV_FOUND_METH:
        amt = G.stat[ST_CLICKER_RPS] * r;
        if (amt < 100) amt = 100;
        G.widgets += amt;
        add_message(MSG_GOOD, "You found an extra barrel of meth, with %s batches inside!", pretty_bigint(amt));
        break;
    case EV_FOUND_CASH:
        amt = (G.stat[ST_SELLER_RPS] * G.widget_roi) * r;
        if (amt < 100) amt = 100;
        earn_cash(amt);
        if (amt > 10000000000.0) add_message(MSG_GOOD, "A mystery benefactor has contributed $%s to your cause", pretty_bigint(amt));
        else if (amt > 10000000) add_message(MSG_GOOD, "You found a truck load of cash, containing $%s inside!", pretty_bigint(amt));
        else if (amt > 100000) add_message(MSG_GOOD, "You found a briefcase with $%s inside!", pretty_int(amt));
        else add_message(MSG_GOOD, "You found some extra cash hidden in a shoe box, worth $%s!", pretty_int(amt));
        break;
    case EV_LOSE_METH:
        amt = G.stat[ST_CLICKER_RPS] * r;
        if (amt < 1) return;
        if (amt > G.widgets) amt = G.widgets;
        G.widgets -= amt;
        add_message(MSG_BAD, "About %s batches of meth have gone missing...", pretty_bigint(amt));
        break;
    case EV_LOSE_CASH:
        if ((amt = cash_loss(r)) > 0) add_message(MSG_BAD, "According to accounting, $%s has been \"lost\"", pretty_bigint(amt));
        break;
    case EV_RIVAL_LOSE_CASH:
        if ((amt = cash_loss(r)) > 0) add_message(MSG_BAD, "A rival cartel has hijacked a sale worth $%s!", pretty_bigint(amt));
        break;
    case EV_PAY_BRIBE:
        if ((amt = cash_loss(r)) > 0) add_message(MSG_BAD, "You had to pay off a government official with $%s", pretty_bigint(amt));
        break;
    case EV_DEA_SEIZE_BUILDING:
        if (!(G.risk_amount > rnd())) { add_message(MSG_GOOD, "You were able to negotiate your way out of a DEA raid"); return; }
        if (bust_ever && now - last_bust < 240000) { add_message(MSG_GOOD, "You narrowly avoided an altercation with the DEA"); return; }
        c = CAT_CLICKERS;
        for (i = 0; i < cat_n[c]; i++)
            if (G.item[c][i].amount > 0 && G.item[c][i].risk > 0.001) picks[np++] = i;
        if (np < 1) { add_message(MSG_GOOD, "The DEA attempted to seize something, but they couldn't find anything to seize"); return; }
        i = picks[(int)floor(rnd() * np)];
        G.item[c][i].amount -= 1;
        add_message(MSG_BAD, "The DEA has seized a %s, it's a sad day for meth addicts everywhere :(", cat_def[c][i].label);
        event_dea_seize_cash(0.1);
        last_bust = now;
        bust_ever = 1;
        break;
    }
}

static void check_events(Uint32 now)
{
    int i;
    for (i = 0; i < LEN(events_def); i++)
        if (events_def[i].chance > rnd()) run_event(&events_def[i], now);
}

/* Actions ---------------------------------------------------------------- */

static double make_click(void)
{
    double amt = click_make_amount();
    do_make(amt);
    G.stat[ST_HAND_MADE] += amt;
    return amt;
}

static double sell_click(void)
{
    double sale = do_sell(click_sell_amount());
    if (sale) G.stat[ST_HAND_SOLD] += sale;
    return sale;
}

static void buy_item(int c, int i)
{
    double cost = item_cost(c, i);
    if (!spend_cash(cost)) return;
    G.item[c][i].amount += 1;
    add_message(MSG_NORMAL, "You have purchased a %s for $%s", cat_def[c][i].label, pretty_bigint(cost));
}

static void sell_item(int c, int i)
{
    double v;
    if (G.item[c][i].amount < 1) return;
    v = item_sell_value(c, i);
    earn_cash(v);
    add_message(MSG_NORMAL, "You sold a %s for $%s", cat_def[c][i].label, pretty_bigint(v));
    G.item[c][i].amount -= 1;
}

static void buy_upgrade(int u)
{
    const UpgradeDef *d = &upgrades_def[u];
    if (G.upg[u] || G.cash < d->cost) return;
    apply_upgrade(u);
    spend_cash(d->cost);
    add_message(MSG_NORMAL, "You have unlocked %s for $%s", d->label, pretty_bigint(d->cost));
}

/* Save / load ---------------------------------------------------------------- */

static const char *save_path = "clickingbad.sav";
static const char *cat_tag[NCAT] = { "cl", "sl", "bn" };

static void do_save(void)
{
    char tmp[1024];
    FILE *f;
    int c, i;
    snprintf(tmp, sizeof tmp, "%s.tmp", save_path);
    if (!(f = fopen(tmp, "w"))) { add_message(MSG_ERROR, "Could not save the game"); return; }
    fprintf(f, "clickingbad 1\ncash %.17g\nsafe %.17g\nwidgets %.17g\n", G.cash, G.safe, G.widgets);
    for (c = 0; c < NCAT; c++)
        for (i = 0; i < cat_n[c]; i++)
            if (G.item[c][i].unlocked) fprintf(f, "%s %s %.17g\n", cat_tag[c], cat_def[c][i].sid, G.item[c][i].amount);
    for (i = 0; i < NUPG; i++)
        if (G.upg[i]) fprintf(f, "u %s\n", upgrades_def[i].sid);
    for (i = 0; i < NACH; i++)
        if (G.ach[i]) fprintf(f, "a %s\n", achievements_def[i].sid);
    for (i = 0; i < NSTAT; i++) fprintf(f, "s %s %.17g\n", stat_names[i], G.stat[i]);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, save_path) != 0) { add_message(MSG_ERROR, "Could not save the game"); return; }
    saved_ago = 0;
}

static int do_load(void)
{
    char line[512], a[128], b[128];
    double v;
    FILE *f = fopen(save_path, "r");
    int c, i;
    if (!f) return 0;
    reset_game(0);
    while (fgets(line, sizeof line, f)) {
        int n = sscanf(line, "%127s %127s %lf", a, b, &v);
        if (n == 2 && sscanf(b, "%lf", &v) == 1) {
            if (!strcmp(a, "cash")) G.cash = v;
            else if (!strcmp(a, "safe")) G.safe = v;
            else if (!strcmp(a, "widgets")) G.widgets = v;
        }
        if (n >= 2 && !strcmp(a, "u")) {
            for (i = 0; i < NUPG; i++)
                if (!strcmp(upgrades_def[i].sid, b)) apply_upgrade(i);
        } else if (n >= 2 && !strcmp(a, "a")) {
            for (i = 0; i < NACH; i++)
                if (!strcmp(achievements_def[i].sid, b)) G.ach[i] = 1;
        } else if (n == 3 && !strcmp(a, "s")) {
            for (i = 0; i < NSTAT; i++)
                if (!strcmp(stat_names[i], b)) G.stat[i] = v;
        } else if (n == 3) {
            for (c = 0; c < NCAT; c++)
                if (!strcmp(a, cat_tag[c]))
                    for (i = 0; i < cat_n[c]; i++)
                        if (!strcmp(cat_def[c][i].sid, b)) {
                            G.item[c][i].amount = v;
                            G.item[c][i].unlocked = 1;
                        }
        }
    }
    fclose(f);
    return 1;
}

/* ---------------------------------------------------------------- rendering helpers */

static SDL_Window *win;
static SDL_Renderer *ren;
static int W, H;
static float S = 1; /* UI scale */

#define NEXTRA 10
static const int extra_cp[NEXTRA] = { 0x2014, 0x2714, 0x2709, 0x2605, 0x2717, 0x2019, 0x25C0, 0x25B6, 0x2022, 0x00B7 };

typedef struct {
    SDL_Texture *tex;
    int tw, th;
    stbtt_packedchar ascii[95], extra[NEXTRA];
    float size, ascent, lh;
} Font;

static unsigned char *ttf_regular, *ttf_bold;
static Font f_small, f_norm, f_bold, f_sbold, f_big, f_btn;
static SDL_Texture *circle_tex;

static unsigned char *read_file(const char *path)
{
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    Sint64 n;
    unsigned char *buf;
    if (!rw) return NULL;
    n = SDL_RWsize(rw);
    buf = malloc((size_t)n);
    if (buf) SDL_RWread(rw, buf, 1, (size_t)n);
    SDL_RWclose(rw);
    return buf;
}

static void font_make(Font *f, const unsigned char *ttf, float px)
{
    stbtt_fontinfo info;
    stbtt_pack_context pc;
    stbtt_pack_range ranges[2];
    unsigned char *bmp;
    Uint32 *rgba;
    int asc, desc, gap, i, tw = 512, th = 256, ok = 0;
    if (f->tex) SDL_DestroyTexture(f->tex);
    memset(f, 0, sizeof *f);
    px = floorf(px + 0.5f);
    stbtt_InitFont(&info, ttf, 0);
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    f->size = px;
    f->ascent = floorf(asc * stbtt_ScaleForPixelHeight(&info, px) + 0.5f);
    f->lh = floorf((asc - desc + gap) * stbtt_ScaleForPixelHeight(&info, px) + 0.5f);
    memset(ranges, 0, sizeof ranges);
    ranges[0].font_size = px;
    ranges[0].first_unicode_codepoint_in_range = 32;
    ranges[0].num_chars = 95;
    ranges[0].chardata_for_range = f->ascii;
    ranges[1].font_size = px;
    ranges[1].array_of_unicode_codepoints = (int *)extra_cp;
    ranges[1].num_chars = NEXTRA;
    ranges[1].chardata_for_range = f->extra;
    for (;;) {
        bmp = calloc((size_t)tw * th, 1);
        stbtt_PackBegin(&pc, bmp, tw, th, 0, 1, NULL);
        ok = stbtt_PackFontRanges(&pc, ttf, 0, ranges, 2);
        stbtt_PackEnd(&pc);
        if (ok || tw >= 4096) break;
        free(bmp);
        if (th < tw) th *= 2; else tw *= 2;
    }
    rgba = malloc((size_t)tw * th * 4);
    for (i = 0; i < tw * th; i++) rgba[i] = 0x00FFFFFFu | ((Uint32)bmp[i] << 24);
    f->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, tw, th);
    SDL_UpdateTexture(f->tex, NULL, rgba, tw * 4);
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    f->tw = tw;
    f->th = th;
    free(rgba);
    free(bmp);
}

static const char *utf8_next(const char *s, int *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return s + 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return s + 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) { *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); return s + 3; }
    *cp = '?';
    return s + 1;
}

static stbtt_packedchar *glyph(Font *f, int cp)
{
    int i;
    if (cp >= 32 && cp < 127) return &f->ascii[cp - 32];
    for (i = 0; i < NEXTRA; i++)
        if (extra_cp[i] == cp) return &f->extra[i];
    return &f->ascii['?' - 32];
}

static float text_w_n(Font *f, const char *s, int n)
{
    const char *e = s + n;
    float w = 0;
    int cp;
    while (s < e && *s) {
        s = utf8_next(s, &cp);
        w += glyph(f, cp)->xadvance;
    }
    return w;
}

static float text_w(Font *f, const char *s) { return text_w_n(f, s, (int)strlen(s)); }

typedef struct { Uint8 r, g, b, a; } Col;
#define RGB(hex) ((Col){ (Uint8)((hex) >> 16), (Uint8)((hex) >> 8), (Uint8)(hex), 255 })
static Col with_a(Col c, Uint8 a) { c.a = a; return c; }

static float draw_text_n(Font *f, float x, float y, const char *s, int n, Col c)
{
    const char *e = s + n;
    float px = floorf(x), py = floorf(y) + f->ascent;
    int cp;
    SDL_SetTextureColorMod(f->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(f->tex, c.a);
    while (s < e && *s) {
        stbtt_packedchar *g;
        SDL_Rect src;
        SDL_FRect dst;
        s = utf8_next(s, &cp);
        g = glyph(f, cp);
        src.x = g->x0; src.y = g->y0; src.w = g->x1 - g->x0; src.h = g->y1 - g->y0;
        dst.x = floorf(px + g->xoff + 0.5f); dst.y = floorf(py + g->yoff + 0.5f);
        dst.w = (float)src.w; dst.h = (float)src.h;
        if (src.w > 0) SDL_RenderCopyF(ren, f->tex, &src, &dst);
        px += g->xadvance;
    }
    return px - floorf(x);
}

static float draw_text(Font *f, float x, float y, const char *s, Col c) { return draw_text_n(f, x, y, s, (int)strlen(s), c); }

/* Word-wrapped text; returns the number of lines. draw=0 only measures. */
static int text_wrap(Font *f, float x, float y, float maxw, const char *s, Col c, int draw)
{
    int lines = 0;
    while (*s) {
        const char *p = s, *brk = NULL;
        float w = 0;
        int cp;
        while (*p) {
            const char *q = utf8_next(p, &cp);
            if (cp == ' ') brk = p;
            w += glyph(f, cp)->xadvance;
            if (w > maxw && p > s) break;
            p = q;
        }
        if (*p && brk) p = brk;
        if (draw) draw_text_n(f, x, y + lines * f->lh, s, (int)(p - s), c);
        lines++;
        s = p;
        while (*s == ' ') s++;
    }
    return lines;
}

static void fill_rect(float x, float y, float w, float h, Col c)
{
    SDL_FRect r = { x, y, w, h };
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRectF(ren, &r);
}

static void make_circle(void)
{
    enum { N = 64 };
    static Uint32 px[N * N];
    int x, y;
    for (y = 0; y < N; y++)
        for (x = 0; x < N; x++) {
            float dx = x + 0.5f - N / 2.0f, dy = y + 0.5f - N / 2.0f;
            float a = N / 2.0f - sqrtf(dx * dx + dy * dy) + 0.5f;
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            px[y * N + x] = 0x00FFFFFFu | ((Uint32)(a * 255) << 24);
        }
    circle_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
    SDL_UpdateTexture(circle_tex, NULL, px, N * 4);
    SDL_SetTextureBlendMode(circle_tex, SDL_BLENDMODE_BLEND);
}

static void fill_rrect(float x, float y, float w, float h, float r, Col c)
{
    static const SDL_Rect q[4] = { { 0, 0, 32, 32 }, { 32, 0, 32, 32 }, { 0, 32, 32, 32 }, { 32, 32, 32, 32 } };
    SDL_FRect d;
    x = floorf(x); y = floorf(y); w = floorf(w); h = floorf(h); r = floorf(r);
    if (r * 2 > w) r = floorf(w / 2);
    if (r * 2 > h) r = floorf(h / 2);
    if (r < 1) { fill_rect(x, y, w, h, c); return; }
    SDL_SetTextureColorMod(circle_tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(circle_tex, c.a);
    d = (SDL_FRect){ x, y, r, r }; SDL_RenderCopyF(ren, circle_tex, &q[0], &d);
    d = (SDL_FRect){ x + w - r, y, r, r }; SDL_RenderCopyF(ren, circle_tex, &q[1], &d);
    d = (SDL_FRect){ x, y + h - r, r, r }; SDL_RenderCopyF(ren, circle_tex, &q[2], &d);
    d = (SDL_FRect){ x + w - r, y + h - r, r, r }; SDL_RenderCopyF(ren, circle_tex, &q[3], &d);
    fill_rect(x + r, y, w - 2 * r, h, c);
    fill_rect(x, y + r, r, h - 2 * r, c);
    fill_rect(x + w - r, y + r, r, h - 2 * r, c);
}

/* Rounded outline: outer box in border colour, inner box in fill colour. */
static void box(float x, float y, float w, float h, float r, float bw, Col fill, Col border)
{
    if (bw <= 0) { fill_rrect(x, y, w, h, r, fill); return; }
    fill_rrect(x, y, w, h, r, border);
    fill_rrect(x + bw, y + bw, w - 2 * bw, h - 2 * bw, r - bw, fill);
}

/* ---------------------------------------------------------------- UI */

static const Col C_BG = { 0x22, 0x88, 0xdd, 255 };
static const Col C_WHITE = { 255, 255, 255, 255 };
static const Col C_TEXT = { 0x33, 0x33, 0x33, 255 };
static const Col C_GREY = { 0x77, 0x77, 0x77, 255 };
static const Col C_LGREY = { 0xaa, 0xaa, 0xaa, 255 };
static const Col C_GREEN = { 0x00, 0xaa, 0x00, 255 };
static const Col C_SEL = { 0xff, 0xcc, 0x00, 255 };

enum { TAB_CLICKERS, TAB_SELLERS, TAB_UPGRADES, TAB_BANKS, TAB_ACH, TAB_MSG, TAB_MISC, NTAB };
static const char *tab_names[NTAB] = { "Manufacturing", "Distribution", "Upgrades", "Laundering", "Achievements", "All Messages", "Options & Stats" };
static const int tab_cat[NTAB] = { CAT_CLICKERS, CAT_SELLERS, CAT_NONE, CAT_BANKS, CAT_NONE, CAT_NONE, CAT_NONE };

enum { R_ITEM, R_UPG, R_ACH, R_MSG, R_BANKINFO, R_OPT, R_STATS, R_HELP, R_CREDITS };
enum { OPT_SAVE, OPT_RESET, OPT_RESET_ALL, OPT_QUIT, NOPT };
static const char *opt_names[NOPT] = { "Save", "Reset", "Reset All", "Quit" };
typedef struct { int kind, idx; float y, h; } Row;

static int tab = TAB_CLICKERS, sel[NTAB];
static float scroll[NTAB], scroll_target[NTAB];
static int follow_sel = 1;
static Row rows[256];
static int nrows;
static int has_pad;
static int confirm; /* 0, OPT_RESET, OPT_RESET_ALL, OPT_QUIT shown as a dialog */
static int quit;

/* Clickable areas, rebuilt every frame. */
enum { H_COOK, H_SELL, H_TAB, H_ROW, H_BUY, H_SELLITEM, H_YES, H_NO };
typedef struct { SDL_FRect r; int act, arg; } Hit;
static Hit hits[256];
static int nhits;
static float list_x, list_y, list_w, list_h;

static void add_hit(float x, float y, float w, float h, int act, int arg)
{
    if (nhits < LEN(hits)) hits[nhits++] = (Hit){ { x, y, w, h }, act, arg };
}

typedef struct { float x, y, dx; Uint32 t0; char text[48]; int sell; } Popup;
static Popup popups[24];
static int last_float;

static void add_popup(float x, float y, const char *text, int sell)
{
    int i, best = 0;
    for (i = 0; i < LEN(popups); i++)
        if (popups[i].t0 < popups[best].t0) best = i;
    popups[best].x = x;
    popups[best].dx = last_float ? 30 * S : -30 * S;
    popups[best].y = y;
    popups[best].t0 = SDL_GetTicks() | 1;
    popups[best].sell = sell;
    last_float = !last_float;
    snprintf(popups[best].text, sizeof popups[best].text, "%s", text);
}

/* A pad button letter in a small circle, so on-screen hints match the device. */
static float badge(float x, float y, const char *label, Col bg)
{
    float d = f_sbold.lh, w = text_w(&f_sbold, label), bw = w + d * 0.5f > d ? w + d * 0.5f : d;
    fill_rrect(x, y, bw, d, d / 2, bg);
    draw_text(&f_sbold, x + (bw - w) / 2, y, label, C_WHITE);
    return bw;
}

static void button(float x, float y, float w, float h, const char *label, const char *pad, int enabled, int act, int arg)
{
    Col bg = enabled ? RGB(0xbbbbbb) : RGB(0x777777), fg = enabled ? C_WHITE : C_LGREY;
    float tw = text_w(&f_sbold, label), bwid = 0;
    fill_rrect(x, y, w, h, 5 * S, bg);
    if (pad && has_pad) bwid = text_w(&f_sbold, pad) + f_sbold.lh * 0.5f + 4 * S;
    if (pad && has_pad) badge(x + (w - tw - bwid) / 2, y + (h - f_sbold.lh) / 2, pad, enabled ? RGB(0x555555) : RGB(0x666666));
    draw_text(&f_sbold, x + (w - tw - bwid) / 2 + bwid, y + (h - f_sbold.lh) / 2, label, fg);
    if (enabled) add_hit(x, y, w, h, act, arg);
}

static void stat_line(float x, float *y, const char *label, const char *val)
{
    float w = draw_text(&f_small, x, *y, label, C_TEXT);
    draw_text(&f_sbold, x + w, *y, val, C_TEXT);
    *y += f_small.lh;
}

/* Draws (or measures, draw=0) one list row; returns its height. */
static float row_draw(Row *r, float x, float y, float w, int selected, int ri, int draw)
{
    float pad = 6 * S, ix = x + pad, iw = w - 2 * pad, cy = y + pad, bh = 26 * S, h;
    char buf[256];
    int n;
    Col card = C_WHITE;

    switch (r->kind) {
    case R_ITEM: {
        int c = tab_cat[tab], i = r->idx;
        const ItemDef *d = &cat_def[c][i];
        double cost = item_cost(c, i);
        float bw = 84 * S;
        h = pad + f_bold.lh + text_wrap(&f_small, 0, 0, iw, d->desc, C_GREY, 0) * f_small.lh + 4 * S + bh + pad;
        if (!draw) return h;
        box(x, y, w, h, 8 * S, selected ? 3 * S : 0, card, C_SEL);
        draw_text(&f_bold, ix, cy, d->label, C_TEXT);
        snprintf(buf, sizeof buf, "%s", pretty_int(G.item[c][i].amount));
        draw_text(&f_bold, ix + iw - text_w(&f_bold, buf), cy, buf, C_GREY);
        cy += f_bold.lh;
        cy += text_wrap(&f_small, ix, cy, iw, d->desc, C_GREY, 1) * f_small.lh + 4 * S;
        snprintf(buf, sizeof buf, "$%s", pretty_bigint(cost));
        n = (int)draw_text(&f_sbold, ix, cy + (bh - f_small.lh) / 2, buf, C_TEXT);
        if (c == CAT_BANKS) snprintf(buf, sizeof buf, "  Launders $%s per second", pretty_bigint(G.item[c][i].rps));
        else snprintf(buf, sizeof buf, "  %s %s per second; %s%% risk", c == CAT_CLICKERS ? "Cooks" : "Sells",
                      pretty_bigint(G.item[c][i].rps), pretty_int(G.item[c][i].risk * 100));
        if (ix + n + text_w(&f_small, buf) < ix + iw - 2 * bw - 8 * S)
            draw_text(&f_small, ix + n, cy + (bh - f_small.lh) / 2, buf, C_TEXT);
        button(ix + iw - 2 * bw - 4 * S, cy, bw, bh, c == CAT_BANKS ? "Buy" : "Purchase", selected ? "X" : NULL, cost <= G.cash, H_BUY, ri);
        button(ix + iw - bw, cy, bw, bh, "Sell", selected ? "Y" : NULL, G.item[c][i].amount >= 1, H_SELLITEM, ri);
        break;
    }
    case R_UPG: {
        const UpgradeDef *d = &upgrades_def[r->idx];
        int bought = G.upg[r->idx];
        float tw;
        h = pad + f_bold.lh + text_wrap(&f_small, 0, 0, iw, d->desc, C_GREY, 0) * f_small.lh + pad;
        if (!bought) h += 4 * S + bh;
        if (!draw) return h;
        box(x, y, w, h, 8 * S, selected ? 3 * S : 0, card, C_SEL);
        if (bought) snprintf(buf, sizeof buf, "\xE2\x9C\x94");
        else snprintf(buf, sizeof buf, "$%s", pretty_bigint(d->cost));
        tw = text_w(&f_bold, buf);
        draw_text(&f_bold, ix + iw - tw, cy, buf, bought ? C_GREEN : C_GREY);
        draw_text(&f_bold, ix, cy, d->label, bought ? C_GREEN : C_TEXT);
        cy += f_bold.lh;
        cy += text_wrap(&f_small, ix, cy, iw, d->desc, C_GREY, 1) * f_small.lh + 4 * S;
        if (!bought) button(ix, cy, 110 * S, bh, "Purchase", selected ? "X" : NULL, G.cash >= d->cost, H_BUY, ri);
        break;
    }
    case R_ACH: {
        const AchDef *d = &achievements_def[r->idx];
        int got = G.ach[r->idx];
        h = pad + f_bold.lh + text_wrap(&f_small, 0, 0, iw, d->desc, C_GREY, 0) * f_small.lh + pad;
        if (!draw) return h;
        /* locked: the web version's 50% opacity card, pre-mixed with the background */
        box(x, y, w, h, 8 * S, selected ? 3 * S : 0, got ? card : RGB(0x90c3ee), C_SEL);
        draw_text(&f_bold, ix, cy, d->label, got ? C_GREEN : RGB(0x4d6f8f));
        text_wrap(&f_small, ix, cy + f_bold.lh, iw, d->desc, got ? C_TEXT : RGB(0x4d6f8f), 1);
        break;
    }
    case R_MSG: {
        static const Col mc[] = { { 0xee, 0xee, 0xee, 255 }, { 0xaa, 0xff, 0xaa, 255 }, { 0xff, 0xcc, 0xcc, 255 }, { 0xff, 0x22, 0x22, 255 } };
        Msg *m = &msgs[r->idx];
        h = text_wrap(m->type == MSG_ERROR ? &f_sbold : &f_small, 0, 0, w - 2 * pad, m->text, C_WHITE, 0) * f_small.lh + 4 * S;
        if (!draw) return h;
        text_wrap(m->type == MSG_ERROR ? &f_sbold : &f_small, x + pad, y + 2 * S, w - 2 * pad, m->text, mc[m->type], 1);
        break;
    }
    case R_BANKINFO: {
        static const char *info = "Laundered cash is the amount of your total cash that is safe from being stolen or seized "
                                  "by the DEA or IRS. The more laundered to total cash you have, the lower the likelihood "
                                  "that you'll be audited. Laundered cash is not a separate resource, so it can never be "
                                  "higher than your total cash.";
        h = 2 * pad + f_bold.lh + f_small.lh + text_wrap(&f_small, 0, 0, iw, info, C_TEXT, 0) * f_small.lh;
        if (!draw) return h;
        box(x, y, w, h, 8 * S, selected ? 3 * S : 0, card, C_SEL);
        draw_text(&f_bold, ix, cy, "Laundering", RGB(0x336699));
        snprintf(buf, sizeof buf, "$%s/sec", pretty_bigint(G.stat[ST_BANK_RPS]));
        draw_text(&f_small, ix + iw - text_w(&f_small, buf), cy, buf, C_TEXT);
        cy += f_bold.lh;
        n = (int)draw_text(&f_small, ix, cy, "Total laundered ", C_TEXT);
        snprintf(buf, sizeof buf, "$%s", pretty_bigint(G.safe));
        draw_text(&f_sbold, ix + n, cy, buf, C_TEXT);
        text_wrap(&f_small, ix, cy + f_small.lh, iw, info, C_TEXT, 1);
        break;
    }
    case R_OPT:
        h = bh + 10 * S;
        if (!draw) return h;
        if (selected) fill_rrect(x, y, w, h, 8 * S, C_SEL);
        button(x + 5 * S, y + 5 * S, w - 10 * S, bh, opt_names[r->idx], selected ? "X" : NULL, 1, H_BUY, ri);
        break;
    case R_STATS: {
        h = 2 * pad + f_bold.lh + 11 * f_small.lh;
        if (!draw) return h;
        box(x, y, w, h, 8 * S, selected ? 3 * S : 0, card, C_SEL);
        draw_text(&f_bold, ix, cy, "Stats", C_TEXT);
        cy += f_bold.lh;
        stat_line(ix, &cy, "Batches cooked: ", pretty_bigint(G.stat[ST_MADE]));
        stat_line(ix, &cy, "Batches hand-cooked: ", pretty_bigint(G.stat[ST_HAND_MADE]));
        stat_line(ix, &cy, "Batches sold: ", pretty_bigint(G.stat[ST_SOLD]));
        stat_line(ix, &cy, "Batches hand-sold: ", pretty_bigint(G.stat[ST_HAND_SOLD]));
        stat_line(ix, &cy, "Total upgrades purchased: ", pretty_int(G.stat[ST_BOUGHT_UPG]));
        snprintf(buf, sizeof buf, "$%s", pretty_bigint(G.stat[ST_TOTAL_CASH]));
        stat_line(ix, &cy, "Total cash earned: ", buf);
        snprintf(buf, sizeof buf, "$%s", pretty_bigint(G.stat[ST_TOTAL_SPENT]));
        stat_line(ix, &cy, "Total cash spent: ", buf);
        stat_line(ix, &cy, "Seconds spent playing: ", pretty_int(G.stat[ST_SECONDS]));
        stat_line(ix, &cy, "Batches made per click: ", pretty_int(G.make_amount));
        stat_line(ix, &cy, "Batches sold per click: ", pretty_int(G.sell_amount));
        snprintf(buf, sizeof buf, "%d seconds ago", saved_ago);
        stat_line(ix, &cy, "Game saved ", buf);
        break;
    }
    case R_HELP: {
        static const char *lines[] = {
            "1. Cook batches of meth", "2. Sell meth", "3. Buy stuff with your drug money", "",
            "A: cook    B: sell", "X: buy / choose    Y: sell the selected item",
            "D-pad up/down: move    L1/R1, left/right: switch tab", "Start: save    Select+Start: quit",
            "Touch screen and mouse work too.",
        };
        int k;
        h = 2 * pad + f_bold.lh + LEN(lines) * f_small.lh;
        if (!draw) return h;
        box(x, y, w, h, 8 * S, selected ? 3 * S : 0, card, C_SEL);
        draw_text(&f_bold, ix, cy, "Instructions", C_TEXT);
        cy += f_bold.lh;
        for (k = 0; k < LEN(lines); k++, cy += f_small.lh) draw_text(&f_small, ix, cy, lines[k], C_TEXT);
        break;
    }
    case R_CREDITS: {
        static const char *cred = "Aaron Meier - Programming and design. Cody Stevens - Name. Dustin Lee - Concept. "
                                  "GitHub contributors - nullism, Pringley, atomicstack, dakotacody, csssuf, "
                                  "TooDumbForAName, gmmeyer, paac, jwalt333. Original game: github.com/nullism/clickingbad (MIT).";
        h = 2 * pad + f_bold.lh + text_wrap(&f_small, 0, 0, iw, cred, C_TEXT, 0) * f_small.lh;
        if (!draw) return h;
        box(x, y, w, h, 8 * S, selected ? 3 * S : 0, card, C_SEL);
        draw_text(&f_bold, ix, cy, "Credits", C_TEXT);
        text_wrap(&f_small, ix, cy + f_bold.lh, iw, cred, C_TEXT, 1);
        break;
    }
    default:
        h = 0;
    }
    return h;
}

static void build_rows(void)
{
    int i, c = tab_cat[tab];
    nrows = 0;
#define PUSH(k, ix) do { if (nrows < LEN(rows)) { rows[nrows].kind = (k); rows[nrows].idx = (ix); nrows++; } } while (0)
    if (tab == TAB_BANKS) PUSH(R_BANKINFO, 0);
    if (c != CAT_NONE) {
        for (i = 0; i < cat_n[c]; i++)
            if (G.item[c][i].unlocked) PUSH(R_ITEM, i);
    } else if (tab == TAB_UPGRADES) {
        for (i = 0; i < NUPG; i++)
            if (upgrade_visible(i)) PUSH(R_UPG, i);
    } else if (tab == TAB_ACH) {
        for (i = 0; i < NACH; i++)
            if (G.ach[i] || !achievements_def[i].hidden) PUSH(R_ACH, i);
    } else if (tab == TAB_MSG) {
        for (i = 0; i < nmsgs; i++) PUSH(R_MSG, i);
    } else {
        for (i = 0; i < NOPT; i++) PUSH(R_OPT, i);
        PUSH(R_STATS, 0);
        PUSH(R_HELP, 0);
        PUSH(R_CREDITS, 0);
    }
#undef PUSH
    if (sel[tab] >= nrows) sel[tab] = nrows - 1;
    if (sel[tab] < 0) sel[tab] = 0;
}

static void tab_counts(int t, int *unl, int *tot)
{
    int i, c = tab_cat[t];
    *unl = *tot = 0;
    if (c != CAT_NONE) {
        for (i = 0; i < cat_n[c]; i++) { (*tot)++; *unl += G.item[c][i].unlocked; }
    } else if (t == TAB_UPGRADES) {
        for (i = 0; i < NUPG; i++) {
            const UpgradeDef *d = &upgrades_def[i];
            if (d->prereq >= 0 && !G.upg[d->prereq]) { (*tot)++; continue; }
            if (d->prereq == -2 && !G.upg[i]) continue;
            (*tot)++;
            *unl += G.upg[i];
        }
    } else if (t == TAB_ACH) {
        for (i = 0; i < NACH; i++) {
            if (!G.ach[i] && achievements_def[i].hidden) continue;
            (*tot)++;
            *unl += G.ach[i];
        }
    } else {
        *tot = -1;
    }
}

static SDL_FRect cook_btn, sell_btn;
static int cook_down, sell_down; /* pressed look */

static void draw_resource(float x, float y, float w, int sell, float *out_h, int draw)
{
    char a[160], b[64], c1[96], c2[96];
    float cy = y, bh = 40 * S, bw = w - 20 * S > 240 * S ? 240 * S : w - 20 * S;
    SDL_FRect *br = sell ? &sell_btn : &cook_btn;
    if (!sell) {
        double sell_rate;
        (void)sell_rate;
        snprintf(a, sizeof a, "Batches (purity is %s)", widget_quality());
        snprintf(b, sizeof b, "%s", pretty_int(G.widgets));
        snprintf(c1, sizeof c1, "%s per second (net)", pretty_int(G.stat[ST_CLICKER_RPS] - G.stat[ST_SELLER_RPS]));
        snprintf(c2, sizeof c2, "%s per second (gross)", pretty_int(G.stat[ST_CLICKER_RPS]));
    } else {
        double rate = G.stat[ST_SELLER_RPS];
        if (G.stat[ST_SELLER_RPS] > G.stat[ST_CLICKER_RPS] && G.widgets < G.stat[ST_SELLER_RPS]) rate = G.stat[ST_CLICKER_RPS];
        snprintf(a, sizeof a, "Cash Money ($%.2f ea)", G.widget_roi);
        snprintf(b, sizeof b, "$%s", pretty_int(G.cash));
        snprintf(c1, sizeof c1, "$%s laundered", pretty_int(safe_cash()));
        snprintf(c2, sizeof c2, "$%s per second", pretty_int(rate * G.widget_roi));
    }
    *out_h = f_norm.lh + f_big.lh + 2 * f_small.lh + 6 * S + bh;
    if (!draw) return;
    draw_text(&f_norm, x + (w - text_w(&f_norm, a)) / 2, cy, a, C_TEXT); cy += f_norm.lh;
    draw_text(&f_big, x + (w - text_w(&f_big, b)) / 2, cy, b, C_TEXT); cy += f_big.lh;
    draw_text(&f_small, x + (w - text_w(&f_small, c1)) / 2, cy, c1, C_TEXT); cy += f_small.lh;
    draw_text(&f_small, x + (w - text_w(&f_small, c2)) / 2, cy, c2, C_TEXT); cy += f_small.lh + 6 * S;
    {
        Col bg = sell ? RGB(0x11cc33) : RGB(0x3399ff), bd = sell ? RGB(0x00aa00) : RGB(0x2277ee);
        int down = sell ? sell_down : cook_down;
        const char *lbl = sell ? "SELL!" : "COOK!";
        float bx = x + (w - bw) / 2, tw = text_w(&f_btn, lbl), bdw = 0, off = down ? 2 * S : 0;
        *br = (SDL_FRect){ bx, cy, bw, bh };
        if (down) { bg.r = (Uint8)(bg.r * 0.85f); bg.g = (Uint8)(bg.g * 0.85f); bg.b = (Uint8)(bg.b * 0.85f); }
        box(bx, cy + off, bw, bh - off, 10 * S, 2 * S, bg, bd);
        if (has_pad) bdw = f_sbold.lh + 6 * S;
        if (has_pad) badge(bx + (bw - tw - bdw) / 2, cy + off + (bh - off - f_sbold.lh) / 2, sell ? "B" : "A", bd);
        draw_text(&f_btn, bx + (bw - tw - bdw) / 2 + bdw, cy + off + (bh - off - f_btn.lh) / 2, lbl, C_WHITE);
        add_hit(bx, cy, bw, bh, sell ? H_SELL : H_COOK, 0);
    }
}

static const char *risk_label(double r)
{
    int i;
    for (i = 0; i < LEN(risk_levels); i++)
        if (r < risk_levels[i].level) return risk_levels[i].label;
    return "";
}

/* Risk lines + last message; returns height. */
static float draw_status(float x, float y, float w, int draw)
{
    char buf[200];
    float cy = y, tw;
    int i;
    for (i = 0; i < 2; i++) {
        double r = i ? G.risk2_amount : G.risk_amount;
        snprintf(buf, sizeof buf, "You have a %s chance of %s (%s%%)", risk_label(r), i ? "an IRS audit" : "a DEA raid", pretty_int(r * 100));
        tw = text_w(&f_small, buf);
        if (draw) draw_text(&f_small, x + (w - tw) / 2, cy, buf, C_WHITE);
        cy += f_small.lh;
    }
    if (nmsgs) {
        static const Col mc[] = { { 0xee, 0xee, 0xee, 255 }, { 0xaa, 0xff, 0xaa, 255 }, { 0xff, 0xcc, 0xcc, 255 }, { 0xff, 0x22, 0x22, 255 } };
        int n = text_wrap(&f_small, 0, 0, w, msgs[0].text, C_WHITE, 0);
        if (n > 2) n = 2;
        if (draw) {
            /* centre a single line, wrap longer ones */
            if (n == 1) draw_text(&f_small, x + (w - text_w(&f_small, msgs[0].text)) / 2, cy + 2 * S, msgs[0].text, mc[msgs[0].type]);
            else text_wrap(&f_small, x, cy + 2 * S, w, msgs[0].text, mc[msgs[0].type], 1);
        }
        cy += 2 * f_small.lh + 2 * S;
    }
    return cy - y;
}

static void draw_tabs(float x, float y, float w, float h)
{
    float tx[NTAB], tw[NTAB], total = 0, off = 0, pad = 10 * S, gap = 4 * S, lb = 0;
    char lbl[NTAB][64];
    int t, unl, tot;
    if (has_pad) lb = text_w(&f_sbold, "L1") + f_sbold.lh * 0.5f + 6 * S;
    for (t = 0; t < NTAB; t++) {
        tab_counts(t, &unl, &tot);
        if (tot >= 0) snprintf(lbl[t], sizeof lbl[t], "%s (%d/%d)", tab_names[t], unl, tot);
        else snprintf(lbl[t], sizeof lbl[t], "%s", tab_names[t]);
        tw[t] = text_w(&f_sbold, lbl[t]) + 2 * pad;
        tx[t] = total;
        total += tw[t] + gap;
    }
    total -= gap;
    {
        float avail = w - 2 * lb;
        if (total <= avail) off = (avail - total) / 2;
        else {
            off = avail / 2 - (tx[tab] + tw[tab] / 2);
            if (off > 0) off = 0;
            if (off < avail - total) off = avail - total;
        }
        SDL_Rect clip = { (int)(x + lb), (int)y, (int)avail, (int)h };
        SDL_RenderSetClipRect(ren, &clip);
        for (t = 0; t < NTAB; t++) {
            float bx = x + lb + off + tx[t];
            int act = t == tab;
            fill_rrect(bx, y, tw[t], h, 5 * S, act ? RGB(0xeeeeee) : RGB(0xbbbbbb));
            draw_text(&f_sbold, bx + pad, y + (h - f_sbold.lh) / 2, lbl[t], act ? C_GREY : C_WHITE);
            add_hit(bx, y, tw[t], h, H_TAB, t);
        }
        SDL_RenderSetClipRect(ren, NULL);
    }
    if (has_pad) {
        badge(x, y + (h - f_sbold.lh) / 2, "L1", RGB(0x1166aa));
        badge(x + w - lb + 6 * S, y + (h - f_sbold.lh) / 2, "R1", RGB(0x1166aa));
    }
}

static void draw_list(float x, float y, float w, float h)
{
    float cy = 0, gap = 6 * S, maxs;
    int i;
    SDL_Rect clip = { (int)x, (int)y, (int)w, (int)h };
    list_x = x; list_y = y; list_w = w; list_h = h;
    build_rows();
    for (i = 0; i < nrows; i++) {
        rows[i].y = cy;
        rows[i].h = row_draw(&rows[i], 0, 0, w, 0, i, 0);
        cy += rows[i].h + gap;
    }
    maxs = cy - gap - h;
    if (maxs < 0) maxs = 0;
    if (follow_sel && nrows) {
        Row *r = &rows[sel[tab]];
        if (r->y < scroll_target[tab]) scroll_target[tab] = r->y - (sel[tab] ? gap * 3 : 0);
        if (r->y + r->h > scroll_target[tab] + h) scroll_target[tab] = r->y + r->h - h + gap * 3;
        if (r->h > h) scroll_target[tab] = r->y;
    }
    if (scroll_target[tab] > maxs) scroll_target[tab] = maxs;
    if (scroll_target[tab] < 0) scroll_target[tab] = 0;
    scroll[tab] += (scroll_target[tab] - scroll[tab]) * 0.3f;
    if (fabsf(scroll[tab] - scroll_target[tab]) < 0.5f) scroll[tab] = scroll_target[tab];

    SDL_RenderSetClipRect(ren, &clip);
    for (i = 0; i < nrows; i++) {
        float ry = y + rows[i].y - floorf(scroll[tab]);
        if (ry + rows[i].h < y || ry > y + h) continue;
        {
            int before = nhits;
            add_hit(x, ry, w, rows[i].h, H_ROW, i);
            row_draw(&rows[i], x, ry, w, i == sel[tab] && tab != TAB_MSG, i, 1);
            /* row buttons outside the visible list area must not be clickable */
            for (; before < nhits; before++) {
                SDL_FRect *r = &hits[before].r;
                if (r->y < y) { r->h -= y - r->y; r->y = y; }
                if (r->y + r->h > y + h) r->h = y + h - r->y;
                if (r->h < 0) r->h = 0;
            }
        }
    }
    if (tab == TAB_MSG && !nmsgs) draw_text(&f_small, x + 6 * S, y + 4 * S, "No new messages", C_WHITE);
    SDL_RenderSetClipRect(ren, NULL);
    /* scroll bar */
    if (maxs > 0) {
        float bh = h * h / (maxs + h), by = y + (h - bh) * (scroll[tab] / maxs);
        fill_rrect(x + w + 2 * S, by, 4 * S, bh, 2 * S, with_a(C_WHITE, 140));
    }
}

static void draw_confirm(void)
{
    static const char *text[] = { "", "Are you sure? You'll lose everything except Achievements.",
                                  "Are you sure? You'll lose everything, including Achievements.", "Quit Clicking Bad? Your game will be saved." };
    float w = W * 0.8f > 460 * S ? 460 * S : W * 0.8f, pad = 14 * S, bh = 32 * S;
    int n = text_wrap(&f_norm, 0, 0, w - 2 * pad, text[confirm], C_TEXT, 0);
    float h = 2 * pad + n * f_norm.lh + 12 * S + bh, x = (W - w) / 2, y = (H - h) / 2;
    nhits = 0;
    fill_rect(0, 0, (float)W, (float)H, (Col){ 0, 0, 0, 140 });
    box(x, y, w, h, 10 * S, 2 * S, C_WHITE, RGB(0xdddddd));
    text_wrap(&f_norm, x + pad, y + pad, w - 2 * pad, text[confirm], C_TEXT, 1);
    button(x + pad, y + h - pad - bh, (w - 3 * pad) / 2, bh, "Yes", "A", 1, H_YES, 0);
    button(x + w / 2 + pad / 2, y + h - pad - bh, (w - 3 * pad) / 2, bh, "No", "B", 1, H_NO, 0);
}

static void draw_frame(void)
{
    float m = 6 * S, tabh = 28 * S;
    nhits = 0;
    SDL_SetRenderDrawColor(ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(ren);

    if (W >= H * 1.45f) {
        /* wide: resources on the left, tabs and list on the right */
        float lw = floorf(W * 0.4f), rh, ph, sh, cy;
        draw_resource(0, 0, lw - 2 * m, 0, &rh, 0);
        ph = 2 * rh + 3 * m;
        sh = draw_status(0, 0, lw - 2 * m, 0);
        cy = (H - (ph + m + sh)) / 2;
        if (cy < m) cy = m;
        fill_rrect(m, cy, lw - 2 * m, ph, 5 * S, C_WHITE);
        fill_rect(m + 5 * S, cy + ph, lw - 2 * m - 10 * S, 2 * S, RGB(0xddddee));
        draw_resource(m, cy + m, lw - 2 * m, 0, &rh, 1);
        draw_resource(m, cy + 2 * m + rh, lw - 2 * m, 1, &rh, 1);
        draw_status(m, cy + ph + m, lw - 2 * m, 1);
        draw_tabs(lw, m, W - lw - m, tabh);
        draw_list(lw, m + tabh + m, W - lw - m - 8 * S, H - (m + tabh + m) - m);
    } else {
        float hw = (W - 2 * m) / 2, rh, ph, sh;
        draw_resource(0, 0, hw, 0, &rh, 0);
        ph = rh + 2 * m;
        fill_rrect(m, m, W - 2 * m, ph, 5 * S, C_WHITE);
        fill_rect(m + 5 * S, m + ph, W - 2 * m - 10 * S, 2 * S, RGB(0xddddee));
        draw_resource(m, 2 * m, hw, 0, &rh, 1);
        draw_resource(m + hw, 2 * m, hw, 1, &rh, 1);
        sh = draw_status(m, m + ph + m, W - 2 * m, 1);
        {
            float ty = m + ph + m + sh + 2 * S;
            draw_tabs(m, ty, W - 2 * m, tabh);
            draw_list(m, ty + tabh + m, W - 2 * m - 8 * S, H - (ty + tabh + m) - m);
        }
    }

    {
        int i;
        Uint32 now = SDL_GetTicks();
        for (i = 0; i < LEN(popups); i++) {
            Popup *p = &popups[i];
            float t;
            if (!p->t0) continue;
            t = (now - p->t0) / 750.0f;
            if (t >= 1) { p->t0 = 0; continue; }
            {
                Col c = p->sell ? RGB(0x55ee55) : RGB(0x5588ff);
                float tw = text_w(&f_btn, p->text), x = p->x - tw / 2 + p->dx * t, y = p->y - 100 * S * t;
                Uint8 a = (Uint8)(255 * (1 - t));
                draw_text(&f_btn, x + 1, y + 1, p->text, (Col){ 0x33, 0x33, 0x33, (Uint8)(a / 2) });
                draw_text(&f_btn, x, y, p->text, with_a(c, a));
            }
        }
    }
    if (confirm) draw_confirm();
    SDL_RenderPresent(ren);
}

static void setup_fonts(void)
{
    int w, h;
    SDL_GetRendererOutputSize(ren, &w, &h);
    W = w;
    H = h;
    if (W >= H * 1.45f) S = H / 480.0f;
    else S = fminf(W / 640.0f, H / 480.0f);
    if (W >= H * 1.45f && W / 854.0f < S) S = W / 854.0f;
    font_make(&f_small, ttf_regular, 13 * S);
    font_make(&f_norm, ttf_regular, 15 * S);
    font_make(&f_bold, ttf_bold, 15 * S);
    font_make(&f_sbold, ttf_bold, 13 * S);
    font_make(&f_big, ttf_bold, 24 * S);
    font_make(&f_btn, ttf_bold, 19 * S);
}

/* ---------------------------------------------------------------- input */

static Uint32 last_click;

static void do_cook(void)
{
    Uint32 now = SDL_GetTicks();
    if (now - last_click < 70) return;
    last_click = now;
    {
        char buf[48];
        double amt = make_click();
        snprintf(buf, sizeof buf, "+%s", pretty_bigint(amt));
        add_popup(cook_btn.x + cook_btn.w / 2, cook_btn.y - 10 * S, buf, 0);
    }
}

static void do_sell_click(void)
{
    Uint32 now = SDL_GetTicks();
    double sale;
    if (now - last_click < 70) return;
    last_click = now;
    sale = sell_click();
    if (sale) {
        char buf[48];
        snprintf(buf, sizeof buf, "$%s", pretty_bigint(sale * G.widget_roi));
        add_popup(sell_btn.x + sell_btn.w / 2, sell_btn.y - 10 * S, buf, 1);
    }
}

static void finish_confirm(int yes)
{
    int what = confirm;
    confirm = 0;
    if (!yes) return;
    if (what == OPT_QUIT) quit = 1;
    else {
        reset_game(what == OPT_RESET);
        nmsgs = 0;
        memset(sel, 0, sizeof sel);
        memset(scroll, 0, sizeof scroll);
        memset(scroll_target, 0, sizeof scroll_target);
        do_save();
        add_message(MSG_NORMAL, what == OPT_RESET ? "Game reset" : "Game reset - all");
    }
}

static void activate_row(int ri, int sell)
{
    Row *r;
    if (ri < 0 || ri >= nrows) return;
    r = &rows[ri];
    sel[tab] = ri;
    switch (r->kind) {
    case R_ITEM:
        if (sell) sell_item(tab_cat[tab], r->idx);
        else buy_item(tab_cat[tab], r->idx);
        break;
    case R_UPG:
        if (!sell) buy_upgrade(r->idx);
        break;
    case R_OPT:
        if (sell) break;
        if (r->idx == OPT_SAVE) { do_save(); add_message(MSG_NORMAL, "Game saved"); }
        else confirm = r->idx;
        break;
    }
}

static void set_tab(int t)
{
    tab = (t + NTAB) % NTAB;
    follow_sel = 1;
    build_rows();
}

static void move_sel(int d)
{
    build_rows();
    if (!nrows) return;
    sel[tab] += d;
    if (sel[tab] < 0) sel[tab] = 0;
    if (sel[tab] >= nrows) sel[tab] = nrows - 1;
    follow_sel = 1;
}

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L1, K_R1, K_START, K_BACK, K_PGUP, K_PGDN, NK };

static void press(int k)
{
    if (confirm) {
        if (k == K_A || k == K_X) finish_confirm(1);
        else if (k == K_B || k == K_Y) finish_confirm(0);
        return;
    }
    switch (k) {
    case K_UP: move_sel(-1); break;
    case K_DOWN: move_sel(1); break;
    case K_PGUP: move_sel(-5); break;
    case K_PGDN: move_sel(5); break;
    case K_LEFT: case K_L1: set_tab(tab - 1); break;
    case K_RIGHT: case K_R1: set_tab(tab + 1); break;
    case K_A: do_cook(); break;
    case K_B: do_sell_click(); break;
    case K_X: build_rows(); activate_row(sel[tab], 0); break;
    case K_Y: build_rows(); activate_row(sel[tab], 1); break;
    case K_START: do_save(); add_message(MSG_NORMAL, "Game saved"); break;
    }
}

/* key repeat for navigation */
static int held[NK];
static Uint32 next_rep[NK];

static void key_state(int k, int down)
{
    if (down && !held[k]) {
        press(k);
        next_rep[k] = SDL_GetTicks() + 350;
    }
    held[k] = down;
}

static void repeat_keys(void)
{
    static const int rep[] = { K_UP, K_DOWN, K_PGUP, K_PGDN };
    Uint32 now = SDL_GetTicks();
    int i;
    for (i = 0; i < LEN(rep); i++)
        if (held[rep[i]] && !confirm && (Sint32)(now - next_rep[rep[i]]) >= 0) {
            press(rep[i]);
            next_rep[rep[i]] = now + 70;
        }
}

static int pad_key(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return K_A;
    case SDL_CONTROLLER_BUTTON_B: return K_B;
    case SDL_CONTROLLER_BUTTON_X: return K_X;
    case SDL_CONTROLLER_BUTTON_Y: return K_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return K_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_R1;
    case SDL_CONTROLLER_BUTTON_START: return K_START;
    case SDL_CONTROLLER_BUTTON_BACK: return K_BACK;
    }
    return -1;
}

static int kb_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return K_UP;
    case SDLK_DOWN: return K_DOWN;
    case SDLK_LEFT: return K_LEFT;
    case SDLK_RIGHT: return K_RIGHT;
    case SDLK_PAGEUP: return K_PGUP;
    case SDLK_PAGEDOWN: return K_PGDN;
    case SDLK_z: case SDLK_SPACE: return K_A;
    case SDLK_x: return K_B;
    case SDLK_RETURN: return K_X;
    case SDLK_BACKSPACE: return K_Y;
    case SDLK_q: return K_L1;
    case SDLK_e: case SDLK_TAB: return K_R1;
    case SDLK_F2: return K_START;
    }
    return -1;
}

static Hit *hit_at(float x, float y)
{
    int i;
    for (i = nhits - 1; i >= 0; i--) {
        SDL_FRect *r = &hits[i].r;
        if (x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h) {
            /* a row is only the background: prefer its buttons */
            return &hits[i];
        }
    }
    return NULL;
}

static void click(Hit *h)
{
    if (!h) return;
    switch (h->act) {
    case H_COOK: do_cook(); break;
    case H_SELL: do_sell_click(); break;
    case H_TAB: set_tab(h->arg); break;
    case H_ROW: sel[tab] = h->arg; break;
    case H_BUY: build_rows(); activate_row(h->arg, 0); break;
    case H_SELLITEM: build_rows(); activate_row(h->arg, 1); break;
    case H_YES: finish_confirm(1); break;
    case H_NO: finish_confirm(0); break;
    }
}

static volatile sig_atomic_t got_signal;
static void on_signal(int s) { (void)s; got_signal = 1; }

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    Uint32 last_tick, last_sec, last_event, last_save;
    float mdown_x = 0, mdown_y = 0, mscroll0 = 0;
    int mdown = 0, dragging = 0, axis_dir = 0;
    Hit mhit = { { 0 }, -1, 0 };
    const char *env;
    int i;

    if (argc > 1) save_path = argv[1];
    if ((env = getenv("CLICKINGBAD_SAVE"))) save_path = env;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    rng_state ^= (Uint64)time(NULL) * 2654435761u;

    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if ((env = getenv("SDL_GAMECONTROLLERCONFIG_FILE"))) SDL_GameControllerAddMappingsFromFile(env);
    {
        SDL_DisplayMode dm;
        int fs = SDL_GetDesktopDisplayMode(0, &dm) == 0 && !getenv("CLICKINGBAD_WINDOW");
        int ww = 640, wh = 480;
        if ((env = getenv("CLICKINGBAD_WINDOW"))) sscanf(env, "%dx%d", &ww, &wh);
        win = SDL_CreateWindow("Clicking Bad", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, fs ? dm.w : ww, fs ? dm.h : wh,
                               (fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE) | SDL_WINDOW_ALLOW_HIGHDPI);
    }
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (getenv("CLICKINGBAD_WINDOW") == NULL) SDL_ShowCursor(SDL_DISABLE);

    ttf_regular = read_file("assets/DejaVuSansCondensed.ttf");
    ttf_bold = read_file("assets/DejaVuSansCondensed-Bold.ttf");
    if (!ttf_regular || !ttf_bold) { fprintf(stderr, "assets/DejaVuSansCondensed*.ttf missing\n"); return 1; }
    make_circle();
    setup_fonts();

    reset_game(0);
    if (do_load()) add_message(MSG_NORMAL, "Game loaded!");
    add_message(MSG_NORMAL, "Welcome to Clicking Bad, bitch.");
    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && SDL_GameControllerOpen(i)) has_pad = 1;

    last_tick = last_sec = last_event = last_save = SDL_GetTicks();
    while (!quit && !got_signal) {
        SDL_Event e;
        Uint32 now;
        int ticks = 0;
        while (SDL_PollEvent(&e)) {
            int k;
            switch (e.type) {
            case SDL_QUIT: quit = 1; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) setup_fonts();
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_GameControllerOpen(e.cdevice.which)) has_pad = 1;
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                if ((k = pad_key(e.cbutton.button)) >= 0) {
                    key_state(k, e.type == SDL_CONTROLLERBUTTONDOWN);
                    if (k == K_A) cook_down = held[K_A];
                    if (k == K_B) sell_down = held[K_B];
                    /* Select+Start is the PortMaster quit combo: save first */
                    if (held[K_BACK] && held[K_START]) { do_save(); quit = 1; }
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    int d = e.caxis.value < -16000 ? -1 : e.caxis.value > 16000 ? 1 : 0;
                    if (d != axis_dir) {
                        if (axis_dir) key_state(axis_dir < 0 ? K_UP : K_DOWN, 0);
                        if (d) key_state(d < 0 ? K_UP : K_DOWN, 1);
                        axis_dir = d;
                    }
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (has_pad) break; /* gptokeyb may also send keys: the pad already covers them */
                if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) {
                    if (confirm) finish_confirm(0); else confirm = OPT_QUIT;
                    break;
                }
                if (e.key.repeat) break;
                if ((k = kb_key(e.key.keysym.sym)) >= 0) {
                    key_state(k, e.type == SDL_KEYDOWN);
                    if (k == K_A) cook_down = held[K_A];
                    if (k == K_B) sell_down = held[K_B];
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button != SDL_BUTTON_LEFT) break;
                {
                    /* mouse coordinates are in window units, the renderer draws in pixels */
                    int ww, wh;
                    SDL_GetWindowSize(win, &ww, &wh);
                    mdown_x = e.button.x * (float)W / ww;
                    mdown_y = e.button.y * (float)H / wh;
                }
                mdown = 1;
                dragging = 0;
                mscroll0 = scroll_target[tab];
                {
                    Hit *h = hit_at(mdown_x, mdown_y);
                    mhit = h ? *h : (Hit){ { 0 }, -1, 0 };
                }
                if (mhit.act == H_COOK) { do_cook(); cook_down = 1; mhit.act = -1; }
                else if (mhit.act == H_SELL) { do_sell_click(); sell_down = 1; mhit.act = -1; }
                break;
            case SDL_MOUSEMOTION:
                if (mdown && !confirm) {
                    int ww, wh;
                    float mx, my;
                    SDL_GetWindowSize(win, &ww, &wh);
                    mx = e.motion.x * (float)W / ww;
                    my = e.motion.y * (float)H / wh;
                    (void)mx;
                    if (!dragging && fabsf(my - mdown_y) > 10 * S && mdown_x >= list_x && mdown_x < list_x + list_w + 10 * S &&
                        mdown_y >= list_y && mdown_y < list_y + list_h)
                        dragging = 1;
                    if (dragging) {
                        follow_sel = 0;
                        scroll_target[tab] = mscroll0 - (my - mdown_y);
                        scroll[tab] = scroll_target[tab];
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button != SDL_BUTTON_LEFT) break;
                if (mdown && !dragging && mhit.act >= 0) {
                    Hit h = mhit;
                    if (h.act == H_ROW || h.act == H_BUY || h.act == H_SELLITEM) follow_sel = 0;
                    click(&h);
                }
                mdown = 0;
                cook_down = held[K_A];
                sell_down = held[K_B];
                break;
            case SDL_MOUSEWHEEL:
                follow_sel = 0;
                scroll_target[tab] -= e.wheel.y * 60 * S;
                break;
            }
        }
        repeat_keys();

        now = SDL_GetTicks();
        while (now - last_tick >= 100) { last_tick += 100; ticks++; }
        if (ticks > 360000) ticks = 360000;
        if (ticks) tick(ticks);
        while (now - last_sec >= 1000) { last_sec += 1000; sec_tick(); }
        if (now - last_event >= 120000) { last_event = now; check_events(now); }
        if (now - last_save >= 30000) { last_save = now; do_save(); }

        draw_frame();
    }
    do_save();
    SDL_Quit();
    return 0;
}
