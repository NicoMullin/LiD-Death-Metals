/* The plugin's search, run on the real BrgGame-Steam.exe (mapped as a file,
 * nothing runs). Every piece must be found exactly once. Only run by
 * `build.py --exe PATH`: the game is not part of this repository.
 *
 * The addresses it prints are for comparing with notes; for game build
 * 5.0.4.2 they are checked too. */
#define LID_DM_NO_DLLMAIN
#include "../dll/lid_deathmetals.c"

#include <stdio.h>

static int fails;

static void show(const char *what, unsigned char *base, const void *at, unsigned long long want)
{
    unsigned long long rva = at ? (unsigned long long)((const unsigned char *)at - base) : 0;
    const char *verdict = !at ? "MISSING" : !want ? "ok" : rva == want ? "ok (as in 5.0.4.2)" : "moved";
    printf("%-26s %s%-10llX %s\n", what, at ? "game+0x" : "        ", rva, verdict);
    if (!at) fails++;
}

int main(int argc, char **argv)
{
    struct pieces pc;
    unsigned char *base;
    HMODULE m;

    if (argc < 2) { puts("usage: realexe PATH-TO-BrgGame-Steam.exe"); return 2; }
    InitializeCriticalSection(&g_log_lock);
    m = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!m) { printf("could not map %s (error %lu)\n", argv[1], GetLastError()); return 2; }
    base = (unsigned char *)((ULONG_PTR)m & ~(ULONG_PTR)3);

    find_pieces(base, &pc, 1);
    show("currency bar slot loop", base, pc.slot, 0x1499ccf);
    show("currency bar icon place", base, pc.icon, 0x1499b9b);
    show("Death Metal sign in text", base, pc.texticon, 0x1113d56);
    show("request lookup error", base, pc.err, 0x10f7ff7);
    show("  L\"ERR-0-100\"", base, is_wstr(pc.err_text, L"ERR-0-100") ? pc.err_text : NULL, 0);
    show("request lookup", base, pc.lookup, 0x10f7f68);
    show("  FName", base, pc.fname_ctor, 0x150090);
    show("handler built", base, pc.built, 0x10f93d7);
    show("Kill Coin payment", base, pc.add_money, 0x1324e90);
    show("  soul", base, pc.soul, 0xf89efd0);
    show("  L\"present_coin\"", base, is_wstr(pc.present_coin, L"present_coin") ? pc.present_coin : NULL, 0);
    show("masters.db number reader", base, pc.get_const, 0x12c4890);
    show("user data", base, pc.user, 0x27d3210);
    show("  copy into a reply", base, pc.user_copy, 0x133e5f0);
    show("Changecash decoder", base, pc.decode, 0xe45530);
    show("  and its clean-up", base, pc.destroy, 0xe1c9e0);
    printf("\nexchange can be switched on: %s\n", pc.exchange_ok ? "yes" : "NO");
    if (!pc.exchange_ok) fails++;
    show("stew pull's normal draw", base, pc.stew, 0x13cc468);
    show("  the normal draw", base, pc.normal_draw, 0x13cc060);
    show("  odds list query", base, pc.odds_query, 0x13b3c60);
    show("  odds rows freed", base, pc.odds_free, 0x109d2f0);
    show("  memory (realloc)", base, pc.game_realloc, 0xdfef0);
    show("stew shop's stew list", base, pc.shop_list, 0x115d420);
    show("  filled by", base, pc.fill_stews, 0x13af1f0);
    show("stew list: adds one, stops", base, pc.stew_ids, 0x13b01b8);
    show("  the add", base, pc.stew_ids_add, 0x203b0);
    show("  the walk", base, pc.stew_ids_loop, 0x13b0140);
    show("pull's check of its stews", base, pc.pull_ids, 0x11648fe);
    show("  the check", base, pc.pull_ids_check, 0x130c020);
    show("pull's payment", base, pc.pay, 0x11649dc);
    show("  pay for one", base, pc.pay_one, 0x1326370);
    show("  the purchase", base, pc.purchase, 0x1326200);
    show("pull's draw into the reply", base, pc.drawn, 0x1164b46);
    show("  the draw of n", base, pc.draw_all, 0x13cc2a0);
    printf("\nrandom stews can be switched on: %s\n", pc.stew_ok ? "yes" : "NO");
    if (!pc.stew_ok) fails++;
    printf(fails ? "\n%d FAILED\n" : "\nall found\n", fails);
    return fails != 0;
}
