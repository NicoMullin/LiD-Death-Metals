/* The Death Metal exchange, end to end, without the game.
 *
 * fake_dispatch below is the offline server's request lookup in the game's own
 * byte shapes: the name lookup, the error path for an unknown name and the
 * place the handler object is built. The plugin's real stubs are put into it
 * by patch_exchange(), and the game functions the exchange calls are stand-ins
 * that record what happened. Built with lid_deathmetals.c included, so it
 * tests the plugin's own code, not a copy. */
#define LID_DM_NO_DLLMAIN
#include "../dll/lid_deathmetals.c"

#include <stdio.h>

/* ------------------------------------------------ the fake dispatcher */

int g_count = 3;                    /* the server's request map is not empty */
void *g_current;
const wchar_t err_text[] = L"ERR-0-100";
const wchar_t *g_err_text_ptr = err_text;

void fake_fname(uint64_t *out, const wchar_t *name, int find, int unused);
int *fake_lookup(uint64_t name);
void *fake_make_handler(int index);
void fake_set_index(void *handler, int index);
void fake_seterr(void *reply, const wchar_t *err);
extern unsigned char fake_dispatch[], d_lookup[], d_err[], d_built[];

__asm__(
    ".text\n"
    ".globl fake_dispatch\n"
    "fake_dispatch:\n"                              /* rcx name, rdx reply, r8 request */
    "    movq %r8, 0x18(%rsp)\n"
    "    movq %rdx, 0x10(%rsp)\n"
    "    movq %rbx, 0x8(%rsp)\n"
    "    pushq %rbp\n"
    "    pushq %rsi\n"
    "    pushq %rdi\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, %rbp\n"
    "    subq $0x60, %rsp\n"
    ".globl d_lookup\n"
    "d_lookup:\n"
    "    .byte 0x48,0x8B,0xF2,0x41,0xB9,0x01,0x00,0x00,0x00,0x48,0x8B,0xD1,0x4D,0x8B,0xF0\n"
    "    .byte 0x48,0x8D,0x4D,0xC0,0x45,0x8B,0xC1,0xE8\n"
    "    .long fake_fname - (. + 4)\n"
    "    .byte 0x44,0x8B,0x0D\n"
    "    .long g_count - (. + 4)\n"
    "    .byte 0x45,0x85,0xC9\n"
    "    je d_err\n"
    "    xorl %r15d, %r15d\n"
    "    movq -0x40(%rbp), %rcx\n"
    "    call fake_lookup\n"
    "    testq %rax, %rax\n"
    "    je d_err\n"
    "    movq %rax, %rdi\n"
    "    movl (%rdi), %ecx\n"
    "    call fake_make_handler\n"
    "    movq %rax, %rbx\n"
    ".globl d_built\n"
    "d_built:\n"
    "    .byte 0x48,0x89,0x5D,0x48,0x48,0x85,0xDB,0x74\n"
    "    .byte 9f - (. + 1)\n"
    "    .byte 0x48,0x89,0x1D\n"
    "    .long g_current - (. + 4)\n"
    "    .byte 0x8B,0x17,0x48,0x8B,0xCB,0xE8\n"
    "    .long fake_set_index - (. + 4)\n"
    "    movq (%rbx), %rax\n"                       /* run it: handler->[+0x80](handler, reply, request) */
    "    movq %rbx, %rcx\n"
    "    movq %rsi, %rdx\n"
    "    movq %r14, %r8\n"
    "    call *0x80(%rax)\n"
    "    jmp 8f\n"
    "9:  xorl %eax, %eax\n"
    "8:  movq 0x30(%rbp), %rbx\n"
    "    addq $0x60, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %rdi\n"
    "    popq %rsi\n"
    "    popq %rbp\n"
    "    ret\n"
    ".globl d_err\n"
    "d_err:\n"
    "    .byte 0x48,0x8D,0x15\n"
    "    .long err_text - (. + 4)\n"
    "    .byte 0x48,0x8B,0xCE,0xE8\n"
    "    .long fake_seterr - (. + 4)\n"
    "    .byte 0x32,0xC0,0x48,0x83,0xC4,0x60,0x41,0x5F,0x41,0x5E,0x5F,0x5E,0x5D,0xC3\n"
);

#define NAME_CHANGECASH 101
#define NAME_VIPCONFIG  202
#define NAME_OTHER      303

void fake_fname(uint64_t *out, const wchar_t *name, int find, int unused)
{
    (void)find; (void)unused;
    *out = wcscmp(name, L"Changecash") == 0 ? NAME_CHANGECASH :
           wcscmp(name, L"Vipconfig") == 0 ? NAME_VIPCONFIG : NAME_OTHER + wcslen(name);
}

static int index_vipconfig = 64;
int *fake_lookup(uint64_t name) { return name == NAME_VIPCONFIG ? &index_vipconfig : NULL; }

/* ---------------------------------------------- a Vipconfig handler */

struct handler { void **methods; int index; };
static int g_vip_runs, g_ok_replies, g_err_replies, g_fail_build;
static const wchar_t *g_last_err;

static char vip_run(void *h, void *reply, void *request) { (void)h; (void)reply; (void)request; g_vip_runs++; return 1; }
static char reply_ok(void *h, void *reply) { (void)h; (void)reply; g_ok_replies++; return 1; }
static char reply_err(void *h, void *reply) { (void)h; (void)reply; g_err_replies++; return 0; }
static void *vip_methods[HANDLER_METHODS];

void *fake_make_handler(int index)
{
    static struct handler h;
    (void)index;
    if (g_fail_build)
        return NULL;
    h.methods = vip_methods;
    h.index = -1;
    return &h;
}
void fake_set_index(void *handler, int index) { ((struct handler *)handler)->index = index; }
void fake_seterr(void *reply, const wchar_t *err) { (void)reply; g_last_err = err; }

/* -------------------------------------- the game functions it calls */

struct object { void **vt; int dirty; };
static void mark_dirty(void *o) { ((struct object *)o)->dirty++; }
static void *dirty_vt[8];

static struct object soul;
static unsigned char user[0x200];
static int g_rate = 5000, g_money_ok = 1, g_param_count, g_decoded, g_destroyed;
static long long g_money_paid;
static unsigned char *g_copied_to;

static void *get_obj(void *request, const void *key)
{
    const wchar_t *const *k = key;
    (void)request;
    return wcscmp(*k, L"param") == 0 ? (void *)&g_param_count : NULL;
}
static void *request_vt[0x260 / 8];
static struct { void **vt; } request = {request_vt};

static void decode(void *obj, void *param) { memcpy((unsigned char *)param + PARAM_MEDAL, obj, 4); g_decoded++; }
static void destroy(void *param) { (void)param; g_destroyed++; }
static int get_const(const wchar_t *name, int *out) { if (wcscmp(name, L"EXCHANGE_RATE") == 0) *out = g_rate; return 1; }
static char add_money(void *s, int amount, const wchar_t *what, const wchar_t *from, int flag)
{
    (void)flag;
    if (s != &soul || wcscmp(what, L"total_get_money") != 0 || !from || !g_money_ok)
        return 0;
    g_money_paid += amount;
    return 1;
}
static void user_copy(void *out, char all) { (void)all; g_copied_to = out; }

static const wchar_t total_get_money[] = L"total_get_money";
static const wchar_t present_coin[] = L"present_coin";

/* ---------------------------------------------------------------- tests */

static int fails;
static void expect(const char *what, long long got, long long want)
{
    printf("%-58s got %lld want %lld  %s\n", what, got, want, got == want ? "ok" : "FAIL");
    if (got != want) fails++;
}

static int get_int(int at) { int v; memcpy(&v, user + at, 4); return v; }
static void set_medals(int free_, int paid) { memcpy(user + USER_FREE, &free_, 4); memcpy(user + USER_PAID, &paid, 4); }

static unsigned char reply[0x600];
typedef char (*dispatch_fn)(const wchar_t *name, void *reply, void *request);

static char send(const wchar_t *name)
{
    g_last_err = NULL;
    return ((dispatch_fn)(void *)fake_dispatch)(name, reply, &request);
}

int main(void)
{
    struct handler *h;

    InitializeCriticalSection(&g_log_lock);
    wcscpy(g_log_path, L"exchange-test.log");
    DeleteFileW(g_log_path);

    vip_methods[HANDLER_RUN] = (void *)vip_run;
    vip_methods[HANDLER_REPLY_OK] = (void *)reply_ok;
    vip_methods[HANDLER_REPLY_ERR] = (void *)reply_err;
    dirty_vt[DIRTY] = (void *)mark_dirty;
    soul.vt = dirty_vt;
    memcpy(user, &(void **){dirty_vt}, sizeof(void *));
    request_vt[REQUEST_GET_OBJ] = (void *)get_obj;

    puts("-- before the plugin");
    expect("Vipconfig runs its own handler", send(L"Vipconfig") == 1 && g_vip_runs == 1, 1);
    expect("Changecash is an unknown request (ERR-0-100)",
           send(L"Changecash") == 0 && g_last_err && wcscmp(g_last_err, L"ERR-0-100") == 0, 1);

    /* the pieces, as find_pieces() finds them in the game */
    g_pc.err = d_err;
    g_pc.err_text = (unsigned char *)err_text;
    g_pc.lookup = d_lookup;
    g_pc.fname_ctor = (void *)fake_fname;
    g_pc.built = d_built;
    g_pc.soul = (unsigned char *)&soul;
    g_pc.total_get_money = (unsigned char *)total_get_money;
    g_pc.present_coin = (unsigned char *)present_coin;
    g_pc.add_money = (void *)add_money;
    g_pc.get_const = (void *)get_const;
    g_pc.user = user;
    g_pc.user_copy = (void *)user_copy;
    g_pc.decode = (void *)decode;
    g_pc.destroy = (void *)destroy;
    g_pc.exchange_ok = 1;
    g_page = page_near(d_err);
    patch_exchange((unsigned char *)fake_dispatch);

    puts("-- with the plugin");
    set_medals(5, 10);
    g_param_count = 7;
    expect("Changecash is answered", send(L"Changecash"), 1);
    h = g_current;
    expect("  through the Vipconfig request's set-up (index 64)", h->index, 64);
    expect("  with the exchange as its handler", h->methods[HANDLER_RUN] == (void *)exchange, 1);
    expect("  the other methods are Vipconfig's", h->methods[HANDLER_REPLY_OK] == (void *)reply_ok, 1);
    expect("  Kill Coins paid: 7 x 5000", g_money_paid, 35000);
    expect("  free Death Metals used first: 5 -> 0", get_int(USER_FREE), 0);
    expect("  then paid ones: 10 -> 8", get_int(USER_PAID), 8);
    expect("  soul and user saved", soul.dirty == 1 && ((struct object *)user)->dirty == 1, 1);
    expect("  new totals copied into the reply's user", g_copied_to == reply + REPLY_USER, 1);
    expect("  request decoded and freed", g_decoded == 1 && g_destroyed == 1, 1);
    expect("  answered with the success reply", g_ok_replies, 1);

    g_param_count = 9;
    expect("asking for more than you hold is refused", send(L"Changecash"), 0);
    expect("  nothing paid, nothing taken",
           g_money_paid == 35000 && get_int(USER_FREE) == 0 && get_int(USER_PAID) == 8, 1);
    expect("  answered with the error reply", g_err_replies, 1);

    g_param_count = 3;
    g_money_ok = 0;
    expect("Kill Coin limit: refused", send(L"Changecash"), 0);
    expect("  no Death Metals taken", get_int(USER_PAID), 8);
    g_money_ok = 1;

    g_param_count = 0;
    expect("asking for 0 is refused", send(L"Changecash"), 0);
    g_param_count = -4;
    expect("asking for a negative number is refused", send(L"Changecash"), 0);
    g_param_count = 2;
    g_rate = 0;
    expect("no exchange rate in masters.db: refused", send(L"Changecash"), 0);
    g_rate = 5000;

    expect("Vipconfig still runs its own handler", send(L"Vipconfig") == 1 && g_vip_runs == 2, 1);
    expect("  with its own methods", ((struct handler *)g_current)->methods == vip_methods, 1);
    expect("other unknown requests still get ERR-0-100",
           send(L"Nonsense") == 0 && g_last_err && wcscmp(g_last_err, L"ERR-0-100") == 0, 1);

    g_fail_build = 1;
    expect("Changecash whose handler cannot be built fails cleanly", send(L"Changecash"), 0);
    g_fail_build = 0;
    expect("  and the next Vipconfig is untouched", send(L"Vipconfig") == 1 && g_vip_runs == 3, 1);
    expect("  with its own methods", ((struct handler *)g_current)->methods == vip_methods, 1);

    g_param_count = 8;
    expect("one more exchange: all 8 paid ones", send(L"Changecash"), 1);
    expect("  paid 8 -> 0", get_int(USER_PAID), 0);
    expect("  Kill Coins paid in total", g_money_paid, 75000);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
