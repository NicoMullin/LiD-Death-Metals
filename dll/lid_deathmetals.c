/*
 * lid-deathmetals.dll for Let It Die - a plugin for the LiD Plugin Loader that
 * brings Death Metals back to the offline game. It changes game code in memory
 * only.
 *
 * The loader (dinput8.dll) loads this from Binaries\Win64\lid-plugins when the
 * game starts. It restores what the online game (4.18) had:
 *
 * 1. Death Metals on the currency bar at the top of the screen.
 *   - The number: the bar draws six slots and slot 0 is Death Metals. The
 *     offline release added a check that skips slot 0; it is removed (8 bytes
 *     become no-ops). The game still fills slot 0 with your Death Metal count.
 *   - The icon: the offline release deleted the one draw call for the Death
 *     Metal icon. A 7-byte instruction just before where it stood is sent
 *     through a small stub that makes that draw with the online game's exact
 *     instructions, then runs the instruction it replaced.
 *
 * 2. The Death Metal sign inside text (#MNEY_DM in masters.db texts, such as a
 *    quest's Death Metal reward). The HUD fills a table of inline icons when it
 *    starts (mStrIconInfo, one entry per icon character). The offline release
 *    pointed the Death Metal entry at the Kill Coin images; the online game
 *    used the Death Metal ones. The two layout fields that entry reads are put
 *    back (0x7ac -> 0x75c, 0x7b4 -> 0x764: two 4-byte numbers).
 *
 * 3. Uncle Death's Death Metal to Kill Coin exchange.
 *   The menu screen for it is still in the game's scripts (the Death Metals
 *   mod puts its menu entry back), but the offline game's built-in server has
 *   no "Changecash" request, so it would answer with an error. This adds one:
 *   - When the server cannot find a request's name and the name is
 *     Changecash, the lookup is run again as "Vipconfig" (the VIP auto-renew
 *     setting), whose reply has exactly the same layout.
 *   - When that request's handler object is built, it gets a copy of its
 *     method table with the handler itself replaced by exchange() below.
 *   - exchange() reads how many Death Metals to change, checks you have them,
 *     pays the Kill Coins (EXCHANGE_RATE each, read from masters.db) with the
 *     game's own function, takes the Death Metals (free ones first), puts your
 *     new totals in the reply and saves, all with the game's own functions.
 *   Real Vipconfig requests are not touched.
 *
 * 4. Random stews, for the Death Metal stew. Every normal stew takes its
 *    decals from ONE queue, rolled ahead and kept in the save, so a second
 *    stew could not have decals of its own. The server's call to that draw goes
 *    through lid_dm_draw(): a stew whose odds list (master_skillgacha.odds_id)
 *    starts with SKLGACH_RANDOM_ gets a fresh pick from its own list, weighted
 *    by the odds, every pull, using the game's own masters.db query and memory
 *    functions. Every other stew is passed to the game's draw unchanged, and
 *    the queue in the save is not touched.
 *    The shop also has to offer the stew: the offline server lists only the
 *    first stew in masters.db that is not a box stew. Where it adds that one
 *    and stops, it goes on to the next instead, so every stew is offered. The
 *    list is written to the log on each visit to the shop.
 *    Multi pulls come from the Stew Multi Pull mod, which marks the request:
 *    every pull of a run says how many are left, so a random stew gives no
 *    decal twice in one run (a normal stew's run takes the queue in the save
 *    as it is, repeats and all). Its first pull also asks for the whole run at
 *    once: then that one request pays for every pull with the game's own
 *    purchase (the price times the count), the game's draw runs that many
 *    times into the reply, and the game saves once.
 *
 * Safety rules:
 *   - Every piece is found by its exact bytes and must be found exactly once.
 *     Anything not found is left alone and named in the log. After a game
 *     update that moves the code, the plugin does nothing at all.
 *   - The exchange is switched on only when every piece it needs was found.
 *   - No network, no registry. The only file it writes is lid-deathmetals.log
 *     next to itself.
 *   - Delete lid-deathmetals.dll and the game is exactly stock again.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "version.h"

/* ------------------------------------------------------------------ log */

static wchar_t g_log_path[MAX_PATH];
static int g_log_started;
static CRITICAL_SECTION g_log_lock;

static void log_line(const char *fmt, ...)
{
    char msg[512];
    SYSTEMTIME t;
    va_list ap;
    int n;
    HANDLE f;
    DWORD written;

    if (!g_log_path[0])
        return;
    GetLocalTime(&t);
    n = snprintf(msg, sizeof msg, "%02u:%02u:%02u  ", t.wHour, t.wMinute, t.wSecond);
    va_start(ap, fmt);
    n += vsnprintf(msg + n, sizeof msg - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof msg - 3)
        n = (int)sizeof msg - 3;
    msg[n++] = '\r';
    msg[n++] = '\n';

    /* the exchange runs on the game's server thread */
    EnterCriticalSection(&g_log_lock);
    /* a fresh log each time the game starts */
    f = CreateFileW(g_log_path, g_log_started ? FILE_APPEND_DATA : GENERIC_WRITE,
                    FILE_SHARE_READ, NULL, g_log_started ? OPEN_ALWAYS : CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        g_log_started = 1;
        WriteFile(f, msg, (DWORD)n, &written, NULL);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

/* ------------------------------------------------------------ helpers */

struct span {
    unsigned char *start;
    size_t size;
};

static int section(unsigned char *base, const char *name, struct span *out)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    WORD i;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (strncmp((const char *)sec->Name, name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
            out->start = base + sec->VirtualAddress;
            out->size = sec->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

/* pattern with a mask: 'x' = must match, '?' = any byte */
static int matches(const unsigned char *p, const unsigned char *pat, const char *mask)
{
    size_t i;
    for (i = 0; mask[i]; i++)
        if (mask[i] == 'x' && p[i] != pat[i])
            return 0;
    return 1;
}

static int find_all(struct span s, const unsigned char *pat, const char *mask, unsigned char **last)
{
    size_t i, n = strlen(mask);
    int hits = 0;
    for (i = 0; i + n <= s.size; i++) {
        if (s.start[i] == pat[0] && matches(s.start + i, pat, mask)) {
            hits++;
            *last = s.start + i;
        }
    }
    return hits;
}

/* the same, for a pattern with no wildcards */
static int find_exact(struct span s, const unsigned char *pat, size_t n, unsigned char **last)
{
    size_t i;
    int hits = 0;
    for (i = 0; i + n <= s.size; i++) {
        if (s.start[i] == pat[0] && memcmp(s.start + i, pat, n) == 0) {
            hits++;
            *last = s.start + i;
        }
    }
    return hits;
}

static int32_t read32(const unsigned char *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* where a rel32 field at `at` points, for an instruction ending at `end` */
static unsigned char *rel_target(const unsigned char *at, const unsigned char *end)
{
    return (unsigned char *)end + read32(at);
}

static int rel32(const unsigned char *next, const unsigned char *target, int32_t *out)
{
    intptr_t d = target - next;
    if (d != (int32_t)d)
        return 0;
    *out = (int32_t)d;
    return 1;
}

static int is_wstr(const unsigned char *p, const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    return p && memcmp(p, s, n) == 0;
}

static int write_code(unsigned char *at, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

/* One page within 2 GB of the game code, for the stubs. */
static unsigned char *g_page;
static size_t g_page_used;

static unsigned char *page_near(unsigned char *code)
{
    const ULONG_PTR step = 0x10000;
    ULONG_PTR origin = (ULONG_PTR)code & ~(step - 1);
    ULONG_PTR d;

    for (d = step; d < 0x70000000; d += step) {
        void *p = VirtualAlloc((void *)(origin - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!p)
            p = VirtualAlloc((void *)(origin + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p)
            return p;
    }
    return NULL;
}

static unsigned char *emit(const unsigned char *code, size_t n)
{
    unsigned char *at;
    if (!g_page || g_page_used + n > 4096)
        return NULL;
    at = g_page + g_page_used;
    memcpy(at, code, n);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    g_page_used = (g_page_used + n + 15) & ~(size_t)15;
    return at;
}

/* call or jmp rel32 from `site` to `to`, padded with no-ops to `len` bytes */
static int branch_to(unsigned char *site, size_t len, unsigned char op, unsigned char *to)
{
    unsigned char code[16];
    int32_t r;
    size_t i;
    if (len < 5 || len > sizeof code || !rel32(site + 5, to, &r))
        return 0;
    code[0] = op;
    memcpy(code + 1, &r, 4);
    for (i = 5; i < len; i++)
        code[i] = 0x90;
    return write_code(site, code, len);
}

/* ------------------------------------------------- the currency bar */

/* The slot loop: cmp esi,5 / je skip / test esi,esi / je skip /
 * mov rax,[rbx+0x68]. The offline release added `test esi,esi / je skip`,
 * which skips slot 0 - Death Metals. Both jumps must go to the same place. */
static const unsigned char SLOT_PATTERN[] = {
    0x83, 0xFE, 0x05, 0x0F, 0x84, 0, 0, 0, 0,
    0x85, 0xF6, 0x0F, 0x84, 0, 0, 0, 0,
    0x48, 0x8B, 0x43, 0x68,
};
static const char SLOT_MASK[] = "xxxxx????xxxx????xxxx";
#define SLOT_SKIP_AT  9     /* test esi,esi / je skip: 8 bytes */
#define SLOT_SKIP_LEN 8

/* Two draws in a row: call [rax+0x268] (the one before the icon), then the
 * next draw's set-up, starting mov rdx,[rbx+0x1728]. The Death Metal icon
 * was drawn between them; the stub goes in at that mov. */
static const unsigned char ICON_PATTERN[] = {
    0xFF, 0x90, 0x68, 0x02, 0x00, 0x00,                         /* call [rax+0x268]      */
    0x48, 0x8B, 0x93, 0x28, 0x17, 0x00, 0x00,                   /* mov rdx,[rbx+0x1728]  */
    0x48, 0x8B, 0x03,                                           /* mov rax,[rbx]         */
    0x4C, 0x8B, 0x8B, 0x20, 0x17, 0x00, 0x00,                   /* mov r9,[rbx+0x1720]   */
    0x44, 0x89, 0x64, 0x24, 0x38,                               /* mov [rsp+0x38],r12d   */
    0x44, 0x89, 0x64, 0x24, 0x30,                               /* mov [rsp+0x30],r12d   */
    0xF3, 0x44, 0x0F, 0x11, 0x44, 0x24, 0x28,                   /* movss [rsp+0x28],xmm8 */
    0xF3, 0x0F, 0x11, 0x7C, 0x24, 0x20,                         /* movss [rsp+0x20],xmm7 */
    0x4D, 0x8B, 0x89, 0xA4, 0x07, 0x00, 0x00,                   /* mov r9,[r9+0x7a4]     */
    0x4C, 0x8B, 0x82, 0xB4, 0x07, 0x00, 0x00,                   /* mov r8,[rdx+0x7b4]    */
};
#define ICON_HOOK_AT  6     /* mov rdx,[rbx+0x1728]: 7 bytes */
#define ICON_HOOK_LEN 7

/* The stub: the online game's Death Metal icon draw, word for word (the
 * same as the draw after it, with the icon's layout fields 0x75c, 0x764 and
 * 0x7ac), in a stack frame of its own, then the instruction it replaced.
 * rbx, r12, xmm7 and xmm8 are kept across calls, so they hold what the online
 * code had at this point. */
static const unsigned char ICON_STUB[] = {
    0x48, 0x83, 0xEC, 0x48,                                     /* sub rsp,0x48          */
    0x48, 0x8B, 0x93, 0x28, 0x17, 0x00, 0x00,                   /* mov rdx,[rbx+0x1728]  */
    0x48, 0x8B, 0x03,                                           /* mov rax,[rbx]         */
    0x4C, 0x8B, 0x8B, 0x20, 0x17, 0x00, 0x00,                   /* mov r9,[rbx+0x1720]   */
    0x44, 0x89, 0x64, 0x24, 0x38,                               /* mov [rsp+0x38],r12d   */
    0x44, 0x89, 0x64, 0x24, 0x30,                               /* mov [rsp+0x30],r12d   */
    0xF3, 0x44, 0x0F, 0x11, 0x44, 0x24, 0x28,                   /* movss [rsp+0x28],xmm8 */
    0xF3, 0x0F, 0x11, 0x7C, 0x24, 0x20,                         /* movss [rsp+0x20],xmm7 */
    0x4D, 0x8B, 0x89, 0xAC, 0x07, 0x00, 0x00,                   /* mov r9,[r9+0x7ac]     */
    0x4C, 0x8B, 0x82, 0x64, 0x07, 0x00, 0x00,                   /* mov r8,[rdx+0x764]    */
    0x48, 0x8B, 0x92, 0x5C, 0x07, 0x00, 0x00,                   /* mov rdx,[rdx+0x75c]   */
    0x48, 0x8B, 0xCB,                                           /* mov rcx,rbx           */
    0xFF, 0x90, 0x70, 0x02, 0x00, 0x00,                         /* call [rax+0x270]      */
    0x48, 0x83, 0xC4, 0x48,                                     /* add rsp,0x48          */
    0x48, 0x8B, 0x93, 0x28, 0x17, 0x00, 0x00,                   /* mov rdx,[rbx+0x1728]  */
    0xC3,                                                       /* ret                   */
};

/* ------------------------------------------- the exchange: game pieces */

/* The server's request lookup failing: lea rdx,[L"ERR-0-100"] / mov rcx,rsi /
 * call set_error / xor al,al / add rsp,0x60 / pop r15 / pop r14 / pop rdi /
 * pop rsi / pop rbp / ret. The hook goes in at the lea (7 bytes). */
static const unsigned char ERR_PATTERN[] = {
    0x48, 0x8D, 0x15, 0, 0, 0, 0,
    0x48, 0x8B, 0xCE,
    0xE8, 0, 0, 0, 0,
    0x32, 0xC0, 0x48, 0x83, 0xC4, 0x60, 0x41, 0x5F, 0x41, 0x5E, 0x5F, 0x5E, 0x5D, 0xC3,
};
static const char ERR_MASK[] = "xxx????xxxx????xxxxxxxxxxxxxx";

/* The start of that lookup: mov rsi,rdx / mov r9d,1 / mov rdx,rcx /
 * mov r14,r8 / lea rcx,[rbp-0x40] / mov r8d,r9d / call FName / mov r9d,[count]
 * / test r9d,r9d. The name's FName is kept at [rbp-0x40]; the lookup can be
 * run again from the mov r9d. */
static const unsigned char LOOKUP_PATTERN[] = {
    0x48, 0x8B, 0xF2, 0x41, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD1, 0x4D, 0x8B, 0xF0,
    0x48, 0x8D, 0x4D, 0xC0, 0x45, 0x8B, 0xC1,
    0xE8, 0, 0, 0, 0,
    0x44, 0x8B, 0x0D, 0, 0, 0, 0,
    0x45, 0x85, 0xC9,
};
static const char LOOKUP_MASK[] = "xxxxxxxxxxxxxxxxxxxxxxx????xxx????xxx";
#define LOOKUP_FNAME_CALL 22
#define LOOKUP_AGAIN      27

/* The handler object just built: mov [rbp+0x48],rbx / test rbx,rbx / je /
 * mov [current],rbx / mov edx,[rdi] / mov rcx,rbx / call. The hook goes in at
 * the first two instructions (7 bytes). */
static const unsigned char BUILT_PATTERN[] = {
    0x48, 0x89, 0x5D, 0x48, 0x48, 0x85, 0xDB, 0x74, 0,
    0x48, 0x89, 0x1D, 0, 0, 0, 0,
    0x8B, 0x17, 0x48, 0x8B, 0xCB, 0xE8,
};
static const char BUILT_MASK[] = "xxxxxxxx?xxx????xxxxxx";
#define BUILT_HOOK_LEN 7

/* The gift box paying Kill Coins: mov [rsp+0x20],r14d / mov r9,[L"present_coin"]
 * / lea r8,[L"total_get_money"] / mov edx,[r13+0x40] / lea rcx,[soul] /
 * call add_money / test al,al / je / lea rcx,[soul] / mov rax,[soul] /
 * call [rax+0x38] */
static const unsigned char MONEY_PATTERN[] = {
    0x44, 0x89, 0x74, 0x24, 0x20,
    0x4C, 0x8B, 0x0D, 0, 0, 0, 0,
    0x4C, 0x8D, 0x05, 0, 0, 0, 0,
    0x41, 0x8B, 0x55, 0x40,
    0x48, 0x8D, 0x0D, 0, 0, 0, 0,
    0xE8, 0, 0, 0, 0,
    0x84, 0xC0, 0x0F, 0x84, 0, 0, 0, 0,
    0x48, 0x8D, 0x0D, 0, 0, 0, 0,
    0x48, 0x8B, 0x05, 0, 0, 0, 0,
    0xFF, 0x50, 0x38,
};
static const char MONEY_MASK[] = "xxxxxxxx????xxx????xxxxxxx????x????xxxx????xxx????xxx????xxx";

/* The coin locker reading a masters.db number: lea rdx,[rbp+0x320] /
 * lea rcx,[L"COINLOCKER_EXPAND_COUNT"] / call get_const_int */
static const unsigned char CONST_PATTERN[] = {
    0x48, 0x8D, 0x95, 0x20, 0x03, 0x00, 0x00, 0x48, 0x8D, 0x0D, 0, 0, 0, 0, 0xE8,
};
static const char CONST_MASK[] = "xxxxxxxxxx????x";

/* Copying your user data into a reply: mov ecx,[paid] / add ecx,[free] /
 * mov [rbx+0x20],ecx / mov eax,[free] / mov [rbx+0x34],eax / mov eax,[paid] /
 * mov [rbx+0x38],eax. 0x99 bytes into that function. */
static const unsigned char USER_PATTERN[] = {
    0x8B, 0x0D, 0, 0, 0, 0, 0x03, 0x0D, 0, 0, 0, 0, 0x89, 0x4B, 0x20,
    0x8B, 0x05, 0, 0, 0, 0, 0x89, 0x43, 0x34,
    0x8B, 0x05, 0, 0, 0, 0, 0x89, 0x43, 0x38,
};
static const char USER_MASK[] = "xx????xx????xxxxx????xxxxx????xxx";
static const unsigned char USER_FUNC_START[] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,
    0x48, 0x89, 0x5C, 0x24, 0x50, 0x0F, 0xB6, 0xFA, 0x48, 0x8B, 0xD9,
};
#define USER_LINES_AT 0x99
#define USER_PAID     0x11C   /* paid Death Metals, in the user object */
#define USER_FREE     0x120   /* free Death Metals */

/* The ChangeCash request decoder, through the engine's native table */
static const char DECODER_NATIVE[] = "UBrgNetworkDeclStructexecGetBrgDbChangeCashParamValue";
static const unsigned char DECODE_CALL[] = {0x48, 0x8B, 0x8C, 0x24, 0xE8, 0x03, 0x00, 0x00, 0xE8};
static const unsigned char DESTROY_CALL[] = {0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8};

#define REPLY_USER        0x15C   /* FBrgDbUser in the Changecash reply */
#define PARAM_SIZE        0x3A0   /* FBrgDbChangeCashParam */
#define PARAM_MEDAL       0x39C
#define HANDLER_METHODS   17      /* +0x00 .. +0x80 */
#define HANDLER_RUN       16      /* +0x80: the request's own work */
#define HANDLER_REPLY_OK  (0x30 / 8)
#define HANDLER_REPLY_ERR (0x68 / 8)
/* --------------------------------------------- the Death Metal sign in text */

/* The HUD's icon set-up, the Death Metal entry (icon character '2'): its draw
 * image and guide image come from the layout fields 0x7ac and 0x7b4 - the
 * Kill Coin's, as the entry for '0' - where the online game read 0x75c and
 * 0x764, the Death Metal images the currency bar uses too.
 *   mov rax,[rbx+0x1548] / mov rcx,[rax+0x2d18] / mov rax,[rcx+0x148] /
 *   mov rcx,[rax+0x7ac] / mov [rbx+0xe68],rcx  (and the same for 0x7b4 ->
 *   [rbx+0xe70]) */
static const unsigned char TEXTICON_PATTERN[] = {
    0x48, 0x8B, 0x83, 0x48, 0x15, 0x00, 0x00, 0x48, 0x8B, 0x88, 0x18, 0x2D, 0x00, 0x00,
    0x48, 0x8B, 0x81, 0x48, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x88, 0xAC, 0x07, 0x00, 0x00,
    0x48, 0x89, 0x8B, 0x68, 0x0E, 0x00, 0x00,
    0x48, 0x8B, 0x83, 0x48, 0x15, 0x00, 0x00, 0x48, 0x8B, 0x88, 0x18, 0x2D, 0x00, 0x00,
    0x48, 0x8B, 0x81, 0x48, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x88, 0xB4, 0x07, 0x00, 0x00,
    0x48, 0x89, 0x8B, 0x70, 0x0E, 0x00, 0x00,
};
#define TEXTICON_DRAW_AT  24    /* the 0x7ac of mov rcx,[rax+0x7ac] */
#define TEXTICON_GUIDE_AT 59    /* the 0x7b4 of mov rcx,[rax+0x7b4] */
#define DM_DRAW_FIELD     0x75c
#define DM_GUIDE_FIELD    0x764

/* ------------------------------------------------ the random stew draw */

/* The server's stew pull, choosing the draw by the stew's type: a box stew's
 * draw, then the normal stew's, each given the stew's odds list name:
 *   mov rdx,[rsi+0x18] / jmp +7 / lea rdx,[L""] / lea r8,[rbp+0x50] /
 *   lea rcx,[rbp-0x28] / call box_draw / mov rbx,rax */
static const unsigned char STEW_BOX_PATTERN[] = {
    0x48, 0x8B, 0x56, 0x18, 0xEB, 0x07, 0x48, 0x8D, 0x15, 0x00, 0x00, 0x00, 0x00,
    0x4C, 0x8D, 0x45, 0x50, 0x48, 0x8D, 0x4D, 0xD8, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD8,
};
static const char STEW_BOX_MASK[] = "xxxxxxxxx????xxxxxxxxx????xxx";
#define STEW_BOX_CALL 21
/*   mov rdx,[rsi+0x18] / jmp +7 / lea rdx,[L""] / lea rcx,[rbp-0x18] /
 *   call normal_draw / mov rbx,rax / lea rax,[rbp-0x38] / cmp rax,rbx / je */
static const unsigned char STEW_PATTERN[] = {
    0x48, 0x8B, 0x56, 0x18, 0xEB, 0x07, 0x48, 0x8D, 0x15, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8D, 0x4D, 0xE8, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0xD8, 0x48, 0x8D, 0x45, 0xC8, 0x48, 0x3B, 0xC3, 0x74,
};
static const char STEW_MASK[] = "xxxxxxxxx????xxxxx????xxxxxxxxxxx";
#define STEW_CALL 17    /* call normal_draw: 5 bytes */

/* Inside the box draw: it reads the stew's odds list with the game's own
 * masters.db query, and frees the rows at the end.
 *   mov rcx,rdx / call odds_query / mov r10,[rax] / xor r9d,r9d / xor r8d,r8d /
 *   lea rdx,[rbp-0x50] / mov rcx,rax / call [r10+0x10] */
static const unsigned char ODDS_READ[] = {
    0x48, 0x8B, 0xCA, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x10, 0x45, 0x33, 0xC9,
    0x45, 0x33, 0xC0, 0x48, 0x8D, 0x55, 0xB0, 0x48, 0x8B, 0xC8, 0x41, 0xFF, 0x52, 0x10,
};
static const char ODDS_READ_MASK[] = "xxxx????xxxxxxxxxxxxxxxxxxxx";
#define ODDS_READ_AT 0x3B
/*   lea rcx,[rbp-0x50] / call free_rows / mov rax,rsi */
static const unsigned char ODDS_FREE[] = {0x48, 0x8D, 0x4D, 0xB0, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC6};
static const char ODDS_FREE_MASK[] = "xxxxx????xxx";
#define ODDS_FREE_AT 0x325
/* Inside the normal draw: copying the decal id it took from the queue.
 *   mov [rbx],rsi / mov [rbx+8],edx / mov [rbx+0xc],edx / add edx,edx /
 *   lea r8d,[rsi+8] / mov rcx,[rbx] / call realloc */
static const unsigned char ALLOC_CALL[] = {
    0x48, 0x89, 0x33, 0x89, 0x53, 0x08, 0x89, 0x53, 0x0C, 0x03, 0xD2,
    0x44, 0x8D, 0x46, 0x08, 0x48, 0x8B, 0x0B, 0xE8, 0x00, 0x00, 0x00, 0x00,
};
static const char ALLOC_CALL_MASK[] = "xxxxxxxxxxxxxxxxxxx????";
#define ALLOC_CALL_AT 0x4A

/* The Mushroom Stew shop's reply: the list of stews on offer is filled, then
 * the user data is copied in.
 *   lea rcx,[r13+0x608] / call fill_stews / lea rcx,[r13+0x15c] / mov dl,1 / call */
static const unsigned char SHOPLIST_PATTERN[] = {
    0x49, 0x8D, 0x8D, 0x08, 0x06, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x8D, 0x8D, 0x5C, 0x01, 0x00, 0x00, 0xB2, 0x01, 0xE8,
};
static const char SHOPLIST_MASK[] = "xxxxxxxx????xxxxxxxxxx";
#define SHOPLIST_CALL 7
#define SHOP_STEW_SIZE 0x20     /* FBrgDbGacha: the stew id first */

/* Which stews the shop offers (getCurrentSkillGachaId): it walks the stews in
 * masters.db, skips box stews, adds the first other one and stops - the offline
 * game offers one normal stew at most.
 *   mov rdx,rsi / call [wcscmp] / test eax,eax / jne add / inc edi / jmp loop /
 *   add: lea rdx,[rbx+8] / mov rcx,r14 / call add_id / mov eax,[rsp+0x30] /
 *   cmp dword [r14+8],0 */
static const unsigned char STEWIDS_PATTERN[] = {
    0x48, 0x8B, 0xD6, 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x04,
    0xFF, 0xC7, 0xEB, 0x00, 0x48, 0x8D, 0x53, 0x08, 0x49, 0x8B, 0xCE, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0x8B, 0x44, 0x24, 0x30, 0x41, 0x83, 0x7E, 0x08, 0x00,
};
static const char STEWIDS_MASK[] = "xxxxx????xxxxxxx?xxxxxxxx????xxxxxxxxx";
#define STEWIDS_JMP  15     /* jmp loop (short) */
#define STEWIDS_CALL 24     /* call add_id: 5 bytes */

/* The stew pull's check of the request's list of the shop's stews (a function
 * that always answers "fine"), before anything is paid or drawn:
 *   cmp dword [rbp+0x444],0 / jle / lea rcx,[rbp+0x43c] / call check / test al,al /
 *   je / mov rax,[r14] / xor r9d,r9d / mov r8d,1003 */
static const unsigned char PULLIDS_PATTERN[] = {
    0x83, 0xBD, 0x44, 0x04, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x48, 0x8D, 0x8D, 0x3C, 0x04, 0x00, 0x00,
    0xE8, 0x00, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x74, 0x00, 0x49, 0x8B, 0x06, 0x45, 0x33, 0xC9,
    0x41, 0xB8, 0xEB, 0x03, 0x00, 0x00,
};
static const char PULLIDS_MASK[] = "xxxxxxxx?xxxxxxxx????xxx?xxxxxxxxxxxx";
#define PULLIDS_CALL 16
/* The Stew Multi Pull mod's NetGacha adds these to that list: on every pull of
 * a multi pull the number of pulls left, and on its first one a request to do
 * them all at once. */
#define MULTI_HINT L"LID_MULTI_LEFT="
#define ALL_HINT L"LID_MULTI_ALL="
#define RUN_MAX 256

/* The stew pull's payment, then (on failure) its error reply:
 *   lea rcx,[rbp-0x20] / call text_of / mov rdx,rax / mov r8,rsi / lea rcx,[user] /
 *   call pay_one / test eax,eax / jns ok / mov rax,[r14] / mov rdx,r13 /
 *   mov rcx,r14 / call [rax+0x68] */
static const unsigned char PAY_PATTERN[] = {
    0x48, 0x8D, 0x4D, 0xE0, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD0, 0x4C, 0x8B, 0xC6,
    0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x79, 0x11,
    0x49, 0x8B, 0x06, 0x49, 0x8B, 0xD5, 0x49, 0x8B, 0xCE, 0xFF, 0x50, 0x68,
};
static const char PAY_MASK[] = "xxxxx????xxxxxxxxx????x????xxxxxxxxxxxxxxxx";
#define PAY_CALL 22
/* pay_one: the game's purchase of one, passed on with a count of 1:
 *   sub rsp,0x38 / mov r9,r8 / mov r8d,1 / mov qword [rsp+0x20],0 / call purchase */
static const unsigned char PAY_ONE_BODY[] = {
    0x48, 0x83, 0xEC, 0x38, 0x4D, 0x8B, 0xC8, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00,
    0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00, 0xE8,
};
#define PAY_ONE_CALL 22
/* Then the draw: the reply's decal list emptied, and `count` decals drawn into it.
 *   lea rbx,[r13+0x5e8] / xor edx,edx / mov rcx,rbx / call empty / mov r8d,1 /
 *   lea rdx,[rbp-0x80] / mov rcx,rbx / call draw_all / mov r8b,1 */
static const unsigned char DRAWN_PATTERN[] = {
    0x49, 0x8D, 0x9D, 0xE8, 0x05, 0x00, 0x00, 0x33, 0xD2, 0x48, 0x8B, 0xCB, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x55, 0x80, 0x48, 0x8B, 0xCB, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0x41, 0xB0, 0x01,
};
static const char DRAWN_MASK[] = "xxxxxxxxxxxxx????xxxxxxxxxxxxxx????xxx";
#define DRAWN_CALL 30
#define DRAW_ALL_REACH 0x400    /* the normal draw call lies this close after draw_all's start */

/* A row of the odds list: the decal id (an FString) and its odds. */
#define ODDS_ROW_SIZE  0x3C
#define ODDS_ROW_SKLID 0x18
#define ODDS_ROW_ODDS  0x28
#define ODDS_ROWS_GET  (0x10 / 8)
/* Stews whose odds list is named like this are drawn fresh every pull. */
#define RANDOM_STEW_PREFIX L"SKLGACH_RANDOM_"

#define REQUEST_GET_OBJ   (0x258 / 8)
#define DIRTY             (0x38 / 8)

struct pieces {
    unsigned char *slot, *icon;                 /* currency bar */
    unsigned char *texticon;                    /* the Death Metal sign in text */
    unsigned char *stew;                        /* the stew pull's normal draw call */
    void *normal_draw, *odds_query, *odds_free, *game_realloc;
    int stew_ok;
    unsigned char *shop_list;                   /* the shop reply's stew list call */
    void *fill_stews;
    unsigned char *stew_ids, *stew_ids_loop;    /* which stews are offered */
    void *stew_ids_add;
    unsigned char *pull_ids;                    /* the pull's check of its stew list */
    void *pull_ids_check;
    unsigned char *pay, *drawn;                 /* the pull's payment and draw calls */
    void *pay_one, *purchase, *draw_all;
    unsigned char *err, *lookup, *built;        /* the server's request handling */
    void *fname_ctor;
    unsigned char *err_text;                    /* L"ERR-0-100" */
    unsigned char *soul, *present_coin, *total_get_money;
    void *add_money, *get_const, *user_copy, *decode, *destroy;
    unsigned char *user;
    int exchange_ok;
};

/* The exe's own idea of where it loads, and its size. In the game every
 * stored pointer is moved to where the exe really is; in a copy mapped by the
 * tests they still use this address. Both are accepted. */
static int image_range(unsigned char *base, ULONG_PTR *preferred, size_t *size)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    *preferred = (ULONG_PTR)nt->OptionalHeader.ImageBase;
    *size = nt->OptionalHeader.SizeOfImage;
    return 1;
}

/* a stored pointer, as an address in this copy of the exe (or NULL) */
static unsigned char *stored_ptr(unsigned char *base, ULONG_PTR v)
{
    ULONG_PTR preferred;
    size_t size;
    image_range(base, &preferred, &size);
    if (v >= (ULONG_PTR)base && v < (ULONG_PTR)base + size)
        return (unsigned char *)v;
    if (v >= preferred && v < preferred + size)
        return base + (v - preferred);
    return NULL;
}

/* the one stored pointer to `target` in `s`, or NULL */
static unsigned char *pointer_to(unsigned char *base, struct span s, unsigned char *target)
{
    ULONG_PTR preferred, v;
    size_t size;
    unsigned char *hit = NULL;

    v = (ULONG_PTR)target;
    if (find_exact(s, (const unsigned char *)&v, sizeof v, &hit) == 1)
        return hit;
    image_range(base, &preferred, &size);
    v = preferred + (ULONG_PTR)(target - base);
    if (v != (ULONG_PTR)target && find_exact(s, (const unsigned char *)&v, sizeof v, &hit) == 1)
        return hit;
    return NULL;
}

static void find_decoder(unsigned char *base, struct pieces *pc)
{
    struct span rdata, data;
    unsigned char *name = NULL, *slot, *fn, *at = NULL;
    size_t i;

    if (!section(base, ".rdata", &rdata) || !section(base, ".data", &data))
        return;
    if (find_exact(rdata, (const unsigned char *)DECODER_NATIVE, sizeof DECODER_NATIVE, &name) != 1)
        return;
    slot = pointer_to(base, data, name);
    if (!slot)
        slot = pointer_to(base, rdata, name);
    if (!slot)
        return;
    {
        ULONG_PTR v;
        memcpy(&v, slot + 8, sizeof v);
        fn = stored_ptr(base, v);
    }
    if (!fn)
        return;
    for (i = 0; i < 0x100; i++) {
        if (!at && memcmp(fn + i, DECODE_CALL, sizeof DECODE_CALL) == 0) {
            at = fn + i;
            pc->decode = rel_target(at + 9, at + 13);
        } else if (at && memcmp(fn + i, DESTROY_CALL, sizeof DESTROY_CALL) == 0) {
            pc->destroy = rel_target(fn + i + 6, fn + i + 10);
            return;
        }
    }
}

static int inside(struct span s, const unsigned char *p, size_t n)
{
    return p >= s.start && p + n <= s.start + s.size;
}

/* The stew pull's draw call and the game functions the random draw uses: all
 * found from the two draws it chooses between, each checked byte for byte. */
static void find_stew(struct span text, struct pieces *pc)
{
    unsigned char *site = NULL, *boxsite = NULL, *box, *normal;

    if (find_all(text, SHOPLIST_PATTERN, SHOPLIST_MASK, &site) == 1) {
        pc->shop_list = site + SHOPLIST_CALL;
        pc->fill_stews = rel_target(site + SHOPLIST_CALL + 1, site + SHOPLIST_CALL + 5);
    }
    if (find_all(text, STEWIDS_PATTERN, STEWIDS_MASK, &site) == 1) {
        pc->stew_ids = site + STEWIDS_CALL;
        pc->stew_ids_add = rel_target(site + STEWIDS_CALL + 1, site + STEWIDS_CALL + 5);
        pc->stew_ids_loop = site + STEWIDS_JMP + 2 + (signed char)site[STEWIDS_JMP + 1];
    }
    if (find_all(text, PULLIDS_PATTERN, PULLIDS_MASK, &site) == 1) {
        pc->pull_ids = site + PULLIDS_CALL;
        pc->pull_ids_check = rel_target(site + PULLIDS_CALL + 1, site + PULLIDS_CALL + 5);
    }
    if (find_all(text, PAY_PATTERN, PAY_MASK, &site) == 1) {
        unsigned char *one = rel_target(site + PAY_CALL + 1, site + PAY_CALL + 5);
        if (inside(text, one, sizeof PAY_ONE_BODY + 4) && memcmp(one, PAY_ONE_BODY, sizeof PAY_ONE_BODY) == 0) {
            pc->pay = site + PAY_CALL;
            pc->pay_one = one;
            pc->purchase = rel_target(one + PAY_ONE_CALL + 1, one + PAY_ONE_CALL + 5);
        }
    }
    if (find_all(text, DRAWN_PATTERN, DRAWN_MASK, &site) == 1) {
        pc->drawn = site + DRAWN_CALL;
        pc->draw_all = rel_target(site + DRAWN_CALL + 1, site + DRAWN_CALL + 5);
    }
    if (find_all(text, STEW_PATTERN, STEW_MASK, &site) != 1 ||
        find_all(text, STEW_BOX_PATTERN, STEW_BOX_MASK, &boxsite) != 1)
        return;
    /* the two draws of the same pull, the box stew's first */
    if (boxsite > site || site - boxsite > 0x200)
        return;
    normal = rel_target(site + STEW_CALL + 1, site + STEW_CALL + 5);
    box = rel_target(boxsite + STEW_BOX_CALL + 1, boxsite + STEW_BOX_CALL + 5);
    if (!inside(text, normal, ALLOC_CALL_AT + sizeof ALLOC_CALL) ||
        !inside(text, box, ODDS_FREE_AT + sizeof ODDS_FREE) ||
        !matches(normal + ALLOC_CALL_AT, ALLOC_CALL, ALLOC_CALL_MASK) ||
        !matches(box + ODDS_READ_AT, ODDS_READ, ODDS_READ_MASK) ||
        !matches(box + ODDS_FREE_AT, ODDS_FREE, ODDS_FREE_MASK))
        return;
    pc->stew = site + STEW_CALL;
    pc->normal_draw = normal;
    pc->game_realloc = rel_target(normal + ALLOC_CALL_AT + 19, normal + ALLOC_CALL_AT + 23);
    pc->odds_query = rel_target(box + ODDS_READ_AT + 4, box + ODDS_READ_AT + 8);
    pc->odds_free = rel_target(box + ODDS_FREE_AT + 5, box + ODDS_FREE_AT + 9);
    pc->stew_ok = 1;
    /* draw_all is the function the normal draw call above is in */
    if (pc->draw_all && !(pc->stew > (unsigned char *)pc->draw_all &&
                          pc->stew - (unsigned char *)pc->draw_all < DRAW_ALL_REACH))
        pc->drawn = NULL, pc->draw_all = NULL;
}

/* Find everything, without changing anything. `base` is the game's exe in
 * memory (the tests pass a mapped copy of it instead). */
static void find_pieces(unsigned char *base, struct pieces *pc, int quiet)
{
    struct span text;
    unsigned char *hit = NULL;
    int n;

    memset(pc, 0, sizeof *pc);
    if (!section(base, ".text", &text))
        return;

    if (find_all(text, SLOT_PATTERN, SLOT_MASK, &hit) == 1 &&
        hit + 9 + read32(hit + 5) == hit + 17 + read32(hit + 13))
        pc->slot = hit;
    if (find_exact(text, ICON_PATTERN, sizeof ICON_PATTERN, &hit) == 1)
        pc->icon = hit;
    if (find_exact(text, TEXTICON_PATTERN, sizeof TEXTICON_PATTERN, &hit) == 1)
        pc->texticon = hit;

    if (find_all(text, ERR_PATTERN, ERR_MASK, &hit) == 1) {
        pc->err = hit;
        pc->err_text = rel_target(hit + 3, hit + 7);
    }
    if (find_all(text, LOOKUP_PATTERN, LOOKUP_MASK, &hit) == 1) {
        pc->lookup = hit;
        pc->fname_ctor = rel_target(hit + LOOKUP_FNAME_CALL + 1, hit + LOOKUP_FNAME_CALL + 5);
    }
    if (find_all(text, BUILT_PATTERN, BUILT_MASK, &hit) == 1)
        pc->built = hit;
    if (find_all(text, MONEY_PATTERN, MONEY_MASK, &hit) == 1) {
        unsigned char *slot = rel_target(hit + 8, hit + 12);
        ULONG_PTR v;
        memcpy(&v, slot, sizeof v);
        pc->present_coin = stored_ptr(base, v);
        pc->total_get_money = rel_target(hit + 15, hit + 19);
        pc->soul = rel_target(hit + 26, hit + 30);
        pc->add_money = rel_target(hit + 31, hit + 35);
    }
    if (find_all(text, CONST_PATTERN, CONST_MASK, &hit) == 1 &&
        is_wstr(rel_target(hit + 10, hit + 14), L"COINLOCKER_EXPAND_COUNT"))
        pc->get_const = rel_target(hit + 15, hit + 19);
    n = find_all(text, USER_PATTERN, USER_MASK, &hit);
    if (n == 1 && memcmp(hit - USER_LINES_AT, USER_FUNC_START, sizeof USER_FUNC_START) == 0) {
        unsigned char *paid = rel_target(hit + 2, hit + 6), *free_ = rel_target(hit + 8, hit + 12);
        if (paid + 4 == free_) {
            pc->user = free_ - USER_FREE;
            pc->user_copy = hit - USER_LINES_AT;
        }
    }
    find_decoder(base, pc);
    find_stew(text, pc);

    pc->exchange_ok = pc->err && is_wstr(pc->err_text, L"ERR-0-100") && pc->lookup && pc->built &&
                      pc->soul && is_wstr(pc->total_get_money, L"total_get_money") &&
                      is_wstr(pc->present_coin, L"present_coin") && pc->add_money && pc->get_const && pc->user && pc->decode &&
                      pc->destroy;
    if (!quiet && !pc->exchange_ok)
        log_line("exchange: pieces found - lookup error %s, lookup %s, handler built %s, Kill Coin "
                 "payment %s, masters.db reader %s, user data %s, request decoder %s",
                 pc->err ? "yes" : "NO", pc->lookup ? "yes" : "NO", pc->built ? "yes" : "NO",
                 pc->add_money ? "yes" : "NO", pc->get_const ? "yes" : "NO", pc->user ? "yes" : "NO",
                 pc->decode && pc->destroy ? "yes" : "NO");
}

/* ------------------------------------------------- the exchange: code */

static struct pieces g_pc;
static uint64_t g_name_changecash, g_name_vipconfig;
static int g_names_ready;
static volatile LONG g_route_next;      /* set for the one request being routed */
static void *g_methods[HANDLER_METHODS];
static int g_methods_ready;

typedef void *(*fname_fn)(void *out, const wchar_t *name, int find, int unused);
typedef char (*reply_fn)(void *handler, void *reply);
typedef void *(*get_obj_fn)(void *request, const void *key);
typedef void (*decode_fn)(void *obj, void *param);
typedef void (*destroy_fn)(void *param);
typedef int (*const_fn)(const wchar_t *name, int *out);
typedef char (*money_fn)(void *soul, int amount, const wchar_t *what, const wchar_t *from, int flag);
typedef void (*user_copy_fn)(void *user_out, char all);
typedef void (*dirty_fn)(void *obj);

static uint64_t fname(const wchar_t *s)
{
    uint64_t out = 0;
    ((fname_fn)g_pc.fname_ctor)(&out, s, 1, 1);
    return out;
}

/* From the hook on the lookup error: is this Changecash? If so, look it up
 * again as Vipconfig and mark the handler that comes out of it. */
int lid_dm_route(unsigned char *frame)
{
    uint64_t name;
    if (!g_names_ready) {
        g_name_changecash = fname(L"Changecash");
        g_name_vipconfig = fname(L"Vipconfig");
        g_names_ready = 1;
    }
    memcpy(&name, frame - 0x40, sizeof name);
    if (name != g_name_changecash || g_name_changecash == g_name_vipconfig)
        return 0;
    memcpy(frame - 0x40, &g_name_vipconfig, sizeof g_name_vipconfig);
    InterlockedExchange(&g_route_next, 1);
    return 1;
}

static char exchange(void *handler, unsigned char *reply, void *request);

/* From the hook where the handler object is built. */
void lid_dm_built(void **handler)
{
    if (!InterlockedExchange(&g_route_next, 0) || !handler)
        return;
    if (!g_methods_ready) {
        memcpy(g_methods, *handler, sizeof g_methods);
        if (!g_methods[HANDLER_RUN]) {
            log_line("exchange: the request handler is not as expected - not changed");
            return;
        }
        g_methods[HANDLER_RUN] = (void *)exchange;
        g_methods_ready = 1;
    }
    *handler = g_methods;
}

static char exchange(void *handler, unsigned char *reply, void *request)
{
    static const struct { const wchar_t *data; int num, max; } key = {L"param", 6, 6};
    void **methods = *(void ***)handler;
    reply_fn ok = (reply_fn)methods[HANDLER_REPLY_OK], fail = (reply_fn)methods[HANDLER_REPLY_ERR];
    unsigned char *user = g_pc.user, param[PARAM_SIZE];
    void *obj;
    int count, rate = 0, free_, paid, take;
    long long coins;

    obj = ((get_obj_fn)(*(void ***)request)[REQUEST_GET_OBJ])(request, &key);
    if (!obj) {
        log_line("exchange: the request has no param - refused");
        return fail(handler, reply);
    }
    memset(param, 0, sizeof param);
    ((decode_fn)g_pc.decode)(obj, param);
    memcpy(&count, param + PARAM_MEDAL, sizeof count);
    ((destroy_fn)g_pc.destroy)(param);

    ((const_fn)g_pc.get_const)(L"EXCHANGE_RATE", &rate);
    memcpy(&free_, user + USER_FREE, 4);
    memcpy(&paid, user + USER_PAID, 4);
    coins = (long long)count * rate;
    if (count <= 0 || rate <= 0 || coins > INT_MAX) {
        log_line("exchange: refused - %d Death Metals at %d Kill Coins each", count, rate);
        return fail(handler, reply);
    }
    if ((long long)free_ + paid < count) {
        log_line("exchange: refused - %d Death Metals asked, %d free + %d paid held", count, free_, paid);
        return fail(handler, reply);
    }
    /* pay first: the game refuses Kill Coins over its limit */
    if (!((money_fn)g_pc.add_money)(g_pc.soul, (int)coins, (const wchar_t *)g_pc.total_get_money,
                                    (const wchar_t *)g_pc.present_coin, 0)) {
        log_line("exchange: refused by the game (Kill Coin limit?) - %lld Kill Coins", coins);
        return fail(handler, reply);
    }
    ((dirty_fn)(*(void ***)g_pc.soul)[DIRTY])(g_pc.soul);

    take = count < free_ ? count : free_;
    free_ -= take;
    paid -= count - take;
    memcpy(user + USER_FREE, &free_, 4);
    memcpy(user + USER_PAID, &paid, 4);
    ((dirty_fn)(*(void ***)user)[DIRTY])(user);

    ((user_copy_fn)g_pc.user_copy)(reply + REPLY_USER, 1);
    log_line("exchange: %d Death Metals -> %lld Kill Coins (left: %d free, %d paid)",
             count, coins, free_, paid);
    return ok(handler, reply);
}

/* ------------------------------------------------ the random stew: code */

struct fstring { wchar_t *data; int num, max; };
struct rows { unsigned char *data; int num, max; };

typedef void *(*draw_fn)(struct fstring *out, const wchar_t *odds_id);
typedef void *(*odds_query_fn)(const wchar_t *odds_id);
typedef void (*odds_get_fn)(void *query, struct rows *out, void *unused1, void *unused2);
typedef void (*odds_free_fn)(struct rows *rows);
typedef void *(*realloc_fn)(void *p, DWORD bytes, DWORD align);

static uint64_t g_rng;

/* xorshift64*, seeded from the clock the first time */
static uint64_t next_random(void)
{
    if (!g_rng) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g_rng = (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ 0x9E3779B97F4A7C15ull;
    }
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

static int is_random_stew(const wchar_t *odds_id)
{
    size_t n = wcslen(RANDOM_STEW_PREFIX);
    return odds_id && wcsncmp(odds_id, RANDOM_STEW_PREFIX, n) == 0;
}

/* The multi pull in progress: how many pulls were left at the last one (0:
 * none), and the decals it has given so far. g_all: this request asks for its
 * whole run at once (0: no); g_draw_n: that many were paid for, to draw. */
static int g_run_left, g_run_n, g_all, g_draw_n;
static wchar_t g_run_ids[RUN_MAX][64];

static int given_this_run(const wchar_t *id)
{
    int i;
    for (i = 0; i < g_run_n; i++)
        if (wcscmp(g_run_ids[i], id) == 0)
            return 1;
    return 0;
}

/* The odds of the row, if it can be drawn (odds above 0; with `fresh`, not
 * given already in this multi pull), and its decal id. */
static int drawable(const unsigned char *row, struct fstring *id, int fresh)
{
    int odds;
    memcpy(&odds, row + ODDS_ROW_ODDS, sizeof odds);
    memcpy(id, row + ODDS_ROW_SKLID, sizeof *id);
    if (odds <= 0 || !id->data || id->num <= 1)
        return 0;
    return fresh && given_this_run(id->data) ? 0 : odds;
}

typedef char (*ids_check_fn)(struct rows *ids);

/* From the stew pull's check of its list of stews, before anything is paid
 * or drawn: a multi pull says how many pulls are left. A single pull, or a
 * count that is not lower than the last one (a new multi pull), starts afresh. */
char lid_dm_pull_ids(struct rows *ids)
{
    size_t n = wcslen(MULTI_HINT), m = wcslen(ALL_HINT);
    struct fstring s;
    int i, left = 0, all = 0;

    for (i = 0; i < ids->num; i++) {
        memcpy(&s, ids->data + (size_t)i * sizeof s, sizeof s);
        if (s.data && s.num > (int)n && wcsncmp(s.data, MULTI_HINT, n) == 0)
            left = _wtoi(s.data + n);
        else if (s.data && s.num > (int)m && wcsncmp(s.data, ALL_HINT, m) == 0)
            all = _wtoi(s.data + m);
    }
    if (left <= 0 || g_run_left <= 0 || left >= g_run_left)
        g_run_n = 0;
    g_run_left = left > 0 ? left : 0;
    g_all = all > 1 && all <= RUN_MAX && g_pc.pay && g_pc.drawn ? all : 0;
    g_draw_n = 0;
    return ((ids_check_fn)g_pc.pull_ids_check)(ids);
}

typedef int (*pay_one_fn)(void *user, const wchar_t *product, void *extra);
typedef int (*purchase_fn)(void *user, const wchar_t *product, int count, void *extra, void *none);
typedef void *(*draw_all_fn)(void *decals, void *stew, int count);

/* In place of the stew pull's payment for one: a request for the whole run
 * pays for every pull at once, with the game's own purchase (the price times
 * the count, or nothing if that is more than the player has). */
int lid_dm_pay(void *user, const wchar_t *product, void *extra)
{
    int paid;
    if (g_all <= 1)
        return ((pay_one_fn)g_pc.pay_one)(user, product, extra);
    paid = ((purchase_fn)g_pc.purchase)(user, product, g_all, extra, NULL);
    if (paid >= 0) {
        g_draw_n = g_all;
        log_line("stew: one request for a run of %d pulls: paid %d x %ls", g_all, g_all,
                 product ? product : L"?");
    } else {
        log_line("stew: a run of %d pulls could not be paid for (%ls) - nothing drawn", g_all,
                 product ? product : L"?");
    }
    return paid;
}

/* In place of the stew pull's draw of one decal: as many as were paid for.
 * The game's draw loops, so each is drawn as a pull of its own would be (a
 * random stew's through lid_dm_draw, without repeats), into the reply. */
void *lid_dm_draw_all(void *decals, void *stew, int count)
{
    if (g_draw_n > 1)
        count = g_draw_n;
    g_draw_n = 0;
    return ((draw_all_fn)g_pc.draw_all)(decals, stew, count);
}

/* In place of the normal stew draw (which takes the next decal from the one
 * queue every normal stew shares, kept in the save): a stew whose odds list is
 * named SKLGACH_RANDOM_* gets a fresh pick from its own list, weighted by the
 * odds, every pull - in a multi pull, not one it gave already, until every
 * decal on the list has come once. Every other stew goes to the game's draw
 * unchanged. */
void *lid_dm_draw(struct fstring *out, const wchar_t *odds_id)
{
    struct rows rows = {0};
    struct fstring id;
    void *query;
    long long total = 0, roll;
    int i, odds, fresh = g_run_left > 0;

    if (!is_random_stew(odds_id))
        return ((draw_fn)g_pc.normal_draw)(out, odds_id);

    out->data = NULL;
    out->num = out->max = 0;
    query = ((odds_query_fn)g_pc.odds_query)(odds_id);
    if (!query) {
        log_line("stew: %ls - the odds list could not be read", odds_id);
        return out;
    }
    ((odds_get_fn)(*(void ***)query)[ODDS_ROWS_GET])(query, &rows, NULL, NULL);
    for (i = 0; i < rows.num; i++)
        total += drawable(rows.data + (size_t)i * ODDS_ROW_SIZE, &id, fresh);
    if (total <= 0 && fresh) {
        /* every decal on the list has come in this multi pull: all again */
        fresh = 0;
        g_run_n = 0;
        for (i = 0; i < rows.num; i++)
            total += drawable(rows.data + (size_t)i * ODDS_ROW_SIZE, &id, fresh);
    }
    if (total <= 0) {
        log_line("stew: %ls has no decal with odds above 0 - nothing drawn", odds_id);
        ((odds_free_fn)g_pc.odds_free)(&rows);
        return out;
    }
    roll = (long long)(next_random() % (uint64_t)total);
    for (i = 0; i < rows.num; i++) {
        odds = drawable(rows.data + (size_t)i * ODDS_ROW_SIZE, &id, fresh);
        if (roll < odds)
            break;
        roll -= odds;
    }
    out->data = ((realloc_fn)g_pc.game_realloc)(NULL, (DWORD)id.num * sizeof(wchar_t), 8);
    if (out->data) {
        memcpy(out->data, id.data, (size_t)id.num * sizeof(wchar_t));
        out->num = out->max = id.num;
        if (g_run_left > 0 && g_run_n < RUN_MAX && wcslen(out->data) < 64)
            wcscpy(g_run_ids[g_run_n++], out->data);
        if (g_run_left > 0)
            log_line("stew: %ls -> %ls (odds %d in %lld; multi pull, %d left)", odds_id, out->data, odds,
                     total, g_run_left);
        else
            log_line("stew: %ls -> %ls (odds %d in %lld)", odds_id, out->data, odds, total);
    } else {
        log_line("stew: %ls - out of memory", odds_id);
    }
    ((odds_free_fn)g_pc.odds_free)(&rows);
    return out;
}

typedef char (*fill_stews_fn)(struct rows *stews);

/* The shop's list of stews on offer, filled by the game, then logged: which
 * stews the menu can show is decided here. */
char lid_dm_shop_list(struct rows *stews)
{
    char ok = ((fill_stews_fn)g_pc.fill_stews)(stews), names[400];
    int i, n = 0;
    struct fstring id;

    names[0] = 0;
    for (i = 0; i < stews->num && n < (int)sizeof names - 80; i++) {
        memcpy(&id, stews->data + (size_t)i * SHOP_STEW_SIZE, sizeof id);
        n += snprintf(names + n, sizeof names - n, "%s%ls", i ? ", " : "",
                      id.data && id.num > 0 ? id.data : L"?");
    }
    log_line("stew shop: %d stew(s) on offer: %s", stews->num, names);
    return ok;
}

/* ------------------------------------------------------ the changes */

static void patch_slot(unsigned char *base)
{
    static const unsigned char nops[SLOT_SKIP_LEN] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    if (!g_pc.slot) {
        log_line("currency bar: slot loop not found exactly once - the Death Metal number is left "
                 "hidden. A game update may have moved it.");
        return;
    }
    /* single-byte no-ops: the jump just before lands on the first of them */
    if (write_code(g_pc.slot + SLOT_SKIP_AT, nops, SLOT_SKIP_LEN))
        log_line("currency bar: slot 0 (Death Metals) is drawn again (game+0x%llX)",
                 (unsigned long long)(g_pc.slot + SLOT_SKIP_AT - base));
}

static void patch_icon(unsigned char *base)
{
    unsigned char *site, *stub;
    if (!g_pc.icon) {
        log_line("currency bar: icon draw place not found exactly once - the Death Metal icon is "
                 "left out. A game update may have moved it.");
        return;
    }
    site = g_pc.icon + ICON_HOOK_AT;
    stub = emit(ICON_STUB, sizeof ICON_STUB);
    if (stub && branch_to(site, ICON_HOOK_LEN, 0xE8, stub))
        log_line("currency bar: the Death Metal icon is drawn again (game+0x%llX)",
                 (unsigned long long)(site - base));
    else
        log_line("currency bar: could not place the icon stub - the Death Metal icon is left out");
}

static void patch_texticon(unsigned char *base)
{
    int32_t draw = DM_DRAW_FIELD, guide = DM_GUIDE_FIELD;
    if (!g_pc.texticon) {
        log_line("text: the Death Metal sign's set-up not found exactly once - it keeps the Kill "
                 "Coin picture. A game update may have moved it.");
        return;
    }
    if (write_code(g_pc.texticon + TEXTICON_DRAW_AT, &draw, 4) &&
        write_code(g_pc.texticon + TEXTICON_GUIDE_AT, &guide, 4))
        log_line("text: the Death Metal sign (#MNEY_DM) shows the Death Metal picture (game+0x%llX)",
                 (unsigned long long)(g_pc.texticon - base));
    else
        log_line("text: could not change the Death Metal sign's set-up");
}

static void put64(unsigned char *at, const void *v) { memcpy(at, &v, 8); }

static void patch_exchange(unsigned char *base)
{
    unsigned char a[64], b[32], *stub_a, *stub_b;
    size_t n;

    if (!g_pc.exchange_ok) {
        log_line("exchange: not every piece was found - the Death Metal exchange stays off");
        return;
    }

    /* A, at the lookup error:  mov rcx,rbp / mov rax,lid_dm_route / call rax /
     * test eax,eax / jz 1f / mov rax,lookup_again / jmp rax /
     * 1: mov rdx,L"ERR-0-100" / mov rax,after_the_lea / jmp rax */
    n = 0;
    memcpy(a + n, "\x48\x89\xE9\x48\xB8", 5); n += 5; put64(a + n, (void *)lid_dm_route); n += 8;
    memcpy(a + n, "\xFF\xD0\x85\xC0\x74\x0C\x48\xB8", 8); n += 8;
    put64(a + n, g_pc.lookup + LOOKUP_AGAIN); n += 8;
    memcpy(a + n, "\xFF\xE0\x48\xBA", 4); n += 4; put64(a + n, g_pc.err_text); n += 8;
    memcpy(a + n, "\x48\xB8", 2); n += 2; put64(a + n, g_pc.err + 7); n += 8;
    memcpy(a + n, "\xFF\xE0", 2); n += 2;
    stub_a = emit(a, n);

    /* B, where the handler is built (called): sub rsp,0x28 / mov rcx,rbx /
     * mov rax,lid_dm_built / call rax / add rsp,0x28 / mov [rbp+0x48],rbx /
     * test rbx,rbx / ret - the flags of the test are what the game checks next */
    n = 0;
    memcpy(b + n, "\x48\x83\xEC\x28\x48\x89\xD9\x48\xB8", 9); n += 9; put64(b + n, (void *)lid_dm_built); n += 8;
    memcpy(b + n, "\xFF\xD0\x48\x83\xC4\x28\x48\x89\x5D\x48\x48\x85\xDB\xC3", 14); n += 14;
    stub_b = emit(b, n);

    if (!stub_a || !stub_b) {
        log_line("exchange: no room for the stubs - the Death Metal exchange stays off");
        return;
    }
    /* B first: until A is in, nothing is ever routed */
    if (!branch_to(g_pc.built, BUILT_HOOK_LEN, 0xE8, stub_b) || !branch_to(g_pc.err, 7, 0xE9, stub_a)) {
        log_line("exchange: could not change the request handling - the Death Metal exchange stays off");
        return;
    }
    log_line("exchange: Changecash requests are answered (game+0x%llX, game+0x%llX)",
             (unsigned long long)(g_pc.err - base), (unsigned long long)(g_pc.built - base));
}

static void patch_stew(unsigned char *base)
{
    unsigned char s[16], *stub;

    /* a call to mov rax,<function> / jmp rax */
    memcpy(s, "\x48\xB8", 2);
    memcpy(s + 10, "\xFF\xE0", 2);

    /* called in place of the normal draw */
    if (!g_pc.stew_ok) {
        log_line("stew: the stew pull's draw not found exactly once - random stews stay off. "
                 "A game update may have moved it.");
    } else {
        put64(s + 2, (void *)lid_dm_draw);
        stub = emit(s, 12);
        if (stub && branch_to(g_pc.stew, 5, 0xE8, stub))
            log_line("stew: stews with an odds list named " "SKLGACH_RANDOM_*" " draw a fresh decal every "
                     "pull (game+0x%llX)", (unsigned long long)(g_pc.stew - base));
        else
            log_line("stew: could not change the stew pull - random stews stay off");
    }

    /* every stew offered, not only the first: the id is added as before, then
     * the walk goes on instead of stopping.
     *   pop rax (the return place: not going back) / mov rax,add_id / call rax /
     *   inc edi / mov rax,loop / jmp rax */
    if (g_pc.stew_ids) {
        unsigned char w[32];
        memcpy(w, "\x58\x48\xB8", 3);
        put64(w + 3, g_pc.stew_ids_add);
        memcpy(w + 11, "\xFF\xD0\xFF\xC7\x48\xB8", 6);
        put64(w + 17, g_pc.stew_ids_loop);
        memcpy(w + 25, "\xFF\xE0", 2);
        stub = emit(w, 27);
        if (stub && branch_to(g_pc.stew_ids, 5, 0xE8, stub))
            log_line("stew shop: every stew in masters.db is offered, not only the first (game+0x%llX)",
                     (unsigned long long)(g_pc.stew_ids - base));
        else
            log_line("stew shop: could not change the stew list - only the first stew is offered");
    } else {
        log_line("stew shop: the stew list not found exactly once - only the first stew is offered. "
                 "A game update may have moved it.");
    }

    /* a multi pull's count, read from the pull's list of stews */
    if (g_pc.pull_ids && g_pc.stew_ok) {
        put64(s + 2, (void *)lid_dm_pull_ids);
        stub = emit(s, 12);
        if (stub && branch_to(g_pc.pull_ids, 5, 0xE8, stub))
            log_line("stew: a multi pull of a random stew gives no decal twice (game+0x%llX)",
                     (unsigned long long)(g_pc.pull_ids - base));
        else
            log_line("stew: could not change the stew pull's check - multi pulls may repeat decals");
    } else {
        log_line("stew: the stew pull's check not found exactly once - multi pulls may repeat decals. "
                 "A game update may have moved it.");
    }

    /* a whole multi pull in one request: paid for and drawn at once. Draw
     * first: until the payment is changed, nothing asks for more than one. */
    if (g_pc.pull_ids && g_pc.stew_ok && g_pc.pay && g_pc.drawn) {
        unsigned char *stub_draw, *stub_pay;
        put64(s + 2, (void *)lid_dm_draw_all);
        stub_draw = emit(s, 12);
        put64(s + 2, (void *)lid_dm_pay);
        stub_pay = emit(s, 12);
        if (stub_draw && stub_pay && branch_to(g_pc.drawn, 5, 0xE8, stub_draw) &&
            branch_to(g_pc.pay, 5, 0xE8, stub_pay))
            log_line("stew: a multi pull is paid for and drawn in one request (game+0x%llX, game+0x%llX)",
                     (unsigned long long)(g_pc.pay - base), (unsigned long long)(g_pc.drawn - base));
        else
            log_line("stew: could not change the stew pull's payment - multi pulls go one pull at a time");
    } else {
        log_line("stew: the stew pull's payment or draw not found exactly once - multi pulls go one "
                 "pull at a time. A game update may have moved them.");
    }

    /* the shop's list of stews, logged on each visit */
    if (g_pc.shop_list) {
        put64(s + 2, (void *)lid_dm_shop_list);
        stub = emit(s, 12);
        if (stub && branch_to(g_pc.shop_list, 5, 0xE8, stub))
            log_line("stew shop: the stews on offer are logged (game+0x%llX)",
                     (unsigned long long)(g_pc.shop_list - base));
    }
}

static void patch_game(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleW(NULL);

    find_pieces(base, &g_pc, 0);
    g_page = page_near(base);
    if (!g_page) {
        log_line("no memory free near the game code - nothing changed");
        return;
    }
    patch_slot(base);
    patch_icon(base);
    patch_texticon(base);
    patch_exchange(base);
    patch_stew(base);
    {
        DWORD old;
        VirtualProtect(g_page, 4096, PAGE_EXECUTE_READ, &old);
    }
    log_line("ready");
}

/* ---------------------------------------------------------------- start */

static int running_in_the_game(void)
{
    wchar_t exe[MAX_PATH];
    const wchar_t *name;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);

    if (n == 0 || n >= MAX_PATH)
        return 0;
    name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    return _wcsicmp(name, L"BrgGame-Steam.exe") == 0;
}

#ifndef LID_DM_NO_DLLMAIN
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t *slash;
        DWORD n;

        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_lock);
        n = GetModuleFileNameW(inst, g_log_path, MAX_PATH);
        slash = (n && n < MAX_PATH - 24) ? wcsrchr(g_log_path, L'\\') : NULL;
        if (slash)
            wcscpy(slash + 1, L"lid-deathmetals.log");
        else
            g_log_path[0] = 0;

        if (!running_in_the_game())
            return TRUE;    /* some other program - change nothing */
        log_line("LiD Death Metals lid-deathmetals.dll " LID_DLL_VERSION " loaded");
        patch_game();
    }
    return TRUE;
}
#endif
