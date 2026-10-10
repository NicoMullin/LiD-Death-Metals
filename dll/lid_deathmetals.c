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
 * The stews (random stews, every stew offered, a multi pull in one request)
 * moved to lid-stewpull.dll (LiD Stew Multi Pull) in 0.6.0.
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

#define REQUEST_GET_OBJ   (0x258 / 8)
#define DIRTY             (0x38 / 8)

struct pieces {
    unsigned char *slot, *icon;                 /* currency bar */
    unsigned char *texticon;                    /* the Death Metal sign in text */
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
