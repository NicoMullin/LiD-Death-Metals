/* Stand-in for a BrgGame-Steam.exe the plugin does not recognise, as after a
 * game update that moves the code. The plugin must change nothing and say so in
 * its log. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char log[8192] = {0};
    HMODULE dll;
    FILE *f;
    int fails = 0;

    dll = LoadLibraryW(L"lid-deathmetals.dll");
    if (!dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }

    f = fopen("lid-deathmetals.log", "rb");
    if (f) { fread(log, 1, sizeof log - 1, f); fclose(f); }
    printf("%s", log);
    if (!strstr(log, "slot loop not found") || !strstr(log, "icon draw place not found") ||
        !strstr(log, "sign's set-up not found") ||
        !strstr(log, "exchange stays off")) {
        puts("FAIL: the log does not say the code was not found");
        fails++;
    }
    if (strstr(log, "again") || strstr(log, "are answered") || strstr(log, "shows the Death Metal")) {
        puts("FAIL: the log says something was changed");
        fails++;
    }
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
