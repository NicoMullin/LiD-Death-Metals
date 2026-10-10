/* Stand-in for BrgGame-Steam.exe: a currency bar and the HUD's Death Metal
 * text-icon set-up in the game's byte shapes.
 * Loads lid-deathmetals.dll from its own folder and checks what it changes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void fake_bar(void *bar);
extern void fake_texticon(void *hud);

__asm__(
@ASM@);

/* what the game holds in the registers the icon draw reads */
int g_r12 = 7;
float g_xmm7 = 2.5f, g_xmm8 = 0.5f;

static int g_cond, g_slots, g_before_calls;
int fake_cond(void) { return g_cond; }
void record_slot(int i) { g_slots |= 1 << i; }

struct draw { uint64_t a, b, c; float d, e; int f, g; };
static struct draw g_draws[8];
static int g_ndraws;

static void before_icon(void *self) { (void)self; g_before_calls++; }
static void draw_image(void *self, uint64_t a, uint64_t b, uint64_t c, float d, float e, int f, int g)
{
    (void)self;
    if (g_ndraws < 8) {
        struct draw x = {a, b, c, d, e, f, g};
        g_draws[g_ndraws] = x;
    }
    g_ndraws++;
}

static void *vtable[0x280 / 8];
static unsigned char layout[0x800], layout2[0x800], other[0x10];
static unsigned char bar[0x1800];
/* the HUD and the chain to its layout object: hud[0x1548] -> a[0x2d18] -> b[0x148] -> res */
static unsigned char hud[0x1600], chain_a[0x2d20], chain_b[0x150], res[0x800];

static void put64(unsigned char *at, uint64_t v) { memcpy(at, &v, 8); }
static void set_ptr(unsigned char *at, const void *p) { memcpy(at, &p, sizeof p); }
static uint64_t rd64(const unsigned char *at) { uint64_t v; memcpy(&v, at, 8); return v; }

/* the layout fields: the icon's (0x75c, 0x764, 0x7ac) and the next draw's */
#define ICON_A 0x1111111111111111ull
#define ICON_B 0x2222222222222222ull
#define ICON_C 0x3333333333333333ull
#define NEXT_A 0x4444444444444444ull
#define NEXT_B 0x5555555555555555ull
#define NEXT_C 0x6666666666666666ull

static int fails;
static void expect(const char *what, unsigned long long got, unsigned long long want)
{
    printf("%-55s got %llX want %llX  %s\n", what, got, want, got == want ? "ok" : "FAIL");
    if (got != want) fails++;
}

static void run(int cond)
{
    g_cond = cond; g_slots = 0; g_ndraws = 0; g_before_calls = 0;
    memset(g_draws, 0, sizeof g_draws);
    fake_bar(bar);
}

static void expect_draw(const char *what, int i, uint64_t a, uint64_t b, uint64_t c)
{
    char label[96];
    snprintf(label, sizeof label, "%s: layout fields", what);
    expect(label, g_draws[i].a == a && g_draws[i].b == b && g_draws[i].c == c, 1);
    snprintf(label, sizeof label, "%s: the game's registers passed on", what);
    expect(label, g_draws[i].d == 2.5f && g_draws[i].e == 0.5f && g_draws[i].f == 7 && g_draws[i].g == 7, 1);
}

int main(void)
{
    HMODULE dll;

    vtable[0x268 / 8] = (void *)before_icon;
    vtable[0x270 / 8] = (void *)draw_image;
    set_ptr(bar, vtable);
    set_ptr(bar + 0x68, other);
    set_ptr(bar + 0x1728, layout);
    set_ptr(bar + 0x1720, layout2);
    put64(layout + 0x75c, ICON_A); put64(layout + 0x764, ICON_B); put64(layout2 + 0x7ac, ICON_C);
    put64(layout + 0x7ac, NEXT_A); put64(layout + 0x7b4, NEXT_B); put64(layout2 + 0x7a4, NEXT_C);

    set_ptr(hud + 0x1548, chain_a); set_ptr(chain_a + 0x2d18, chain_b); set_ptr(chain_b + 0x148, res);
    put64(res + 0x75c, ICON_A); put64(res + 0x764, ICON_B);          /* Death Metal images */
    put64(res + 0x7ac, NEXT_A); put64(res + 0x7b4, NEXT_B);          /* Kill Coin images   */

    puts("-- before the plugin (slots are bits: slot 0 = Death Metals)");
    fake_texticon(hud);
    expect("text sign '2' draw image (stock: the Kill Coin's)", rd64(hud + 0xe68), NEXT_A);
    expect("text sign '2' guide image (stock: the Kill Coin's)", rd64(hud + 0xe70), NEXT_B);
    run(0);
    expect("slots drawn (stock: no slot 0)", g_slots, 0x3E);
    expect("images drawn (stock: no Death Metal icon)", g_ndraws, 1);
    expect_draw("the next draw", 0, NEXT_A, NEXT_B, NEXT_C);
    run(1);
    expect("slots drawn when slot 5 is off (stock)", g_slots, 0x1E);

    dll = LoadLibraryW(L"lid-deathmetals.dll");
    if (!dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
    puts("-- with the plugin");

    run(0);
    expect("slots drawn: slot 0 is back", g_slots, 0x3F);
    expect("the draw before the icon still runs once", g_before_calls, 1);
    expect("images drawn: the icon, then the next one", g_ndraws, 2);
    expect_draw("the Death Metal icon", 0, ICON_A, ICON_B, ICON_C);
    expect_draw("the next draw, unchanged", 1, NEXT_A, NEXT_B, NEXT_C);
    run(1);
    expect("slots drawn when slot 5 is off", g_slots, 0x1F);
    run(0);
    expect("a second frame draws the same", g_slots == 0x3F && g_ndraws == 2, 1);
    memset(hud + 0xe68, 0, 16);
    fake_texticon(hud);
    expect("text sign '2' draw image: the Death Metal's", rd64(hud + 0xe68), ICON_A);
    expect("text sign '2' guide image: the Death Metal's", rd64(hud + 0xe70), ICON_B);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
