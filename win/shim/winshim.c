/* NetHack 3.6  winshim.c */
/* Copyright (c) Adam Powers, 2020 */
/* NetHack may be freely redistributed.  See license for details. */

/* Shim window port for NetHack 3.6 WASM builds.
 * Adapted from the NetHack 3.7 winshim.c by Adam Powers.
 * All window system operations are routed through a single JavaScript
 * callback via Emscripten's Asyncify mechanism.
 */

#include "hack.h"
#include <string.h>

#ifdef SHIM_GRAPHICS
#include <stdarg.h>
/* for cross-compiling to WebAssembly (WASM) */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#undef SHIM_DEBUG

#ifdef SHIM_DEBUG
#define debugf printf
#else /* !SHIM_DEBUG */
#define debugf(...)
#endif /* SHIM_DEBUG */

#define NH3D_TRACKED_MONSTER_NONE (-1)
#define NH3D_TRACKED_PLAYER_ID 0
#define NH3D_MONSTER_ATTACK_TRACKING_MAX 128
#define NH3D_MONSTER_KILLER_STACK_MAX 8

struct nh3d_recent_attack {
    unsigned attacker_id;
    int target_id;
    long turn;
};

static struct nh3d_recent_attack
    nh3d_recent_attacks[NH3D_MONSTER_ATTACK_TRACKING_MAX];
static int nh3d_monster_killer_stack[NH3D_MONSTER_KILLER_STACK_MAX];
static int nh3d_monster_killer_depth = 0;

static boolean nh3d_monster_has_trackable_glyph(struct monst *);
static int nh3d_monster_id_from_tile(XCHAR_P, XCHAR_P, int);
static int nh3d_get_recent_attack_target_id(int);
static void nh3d_record_recent_attack(unsigned, int);
static void nh3d_queue_shim_notification(const char *, int, int, int, int);

static boolean
nh3d_monster_has_trackable_glyph(mtmp)
struct monst *mtmp;
{
    return (boolean) (mtmp && (mtmp == u.usteed || canspotmon(mtmp)));
}

static int
nh3d_monster_id_from_tile(x, y, glyph)
XCHAR_P x, y;
int glyph;
{
    struct monst *mtmp;

    if (glyph_is_ridden_monster(glyph) && u.usteed && x == u.ux && y == u.uy)
        return (int) u.usteed->m_id;
    if (x == u.ux && y == u.uy)
        return NH3D_TRACKED_PLAYER_ID;
    if (!glyph_is_monster(glyph))
        return NH3D_TRACKED_MONSTER_NONE;

    mtmp = m_at((int) x, (int) y);
    return mtmp ? (int) mtmp->m_id : NH3D_TRACKED_MONSTER_NONE;
}

static int
nh3d_get_recent_attack_target_id(monster_id)
int monster_id;
{
    int i;

    if (monster_id <= 0)
        return NH3D_TRACKED_MONSTER_NONE;
    for (i = 0; i < NH3D_MONSTER_ATTACK_TRACKING_MAX; ++i)
        if (nh3d_recent_attacks[i].attacker_id == (unsigned) monster_id
            && nh3d_recent_attacks[i].turn == monstermoves)
            return nh3d_recent_attacks[i].target_id;
    return NH3D_TRACKED_MONSTER_NONE;
}

static void
nh3d_record_recent_attack(attacker_id, target_id)
unsigned attacker_id;
int target_id;
{
    int i, empty_slot = -1;

    if (!attacker_id)
        return;

    for (i = 0; i < NH3D_MONSTER_ATTACK_TRACKING_MAX; ++i) {
        if (nh3d_recent_attacks[i].attacker_id == attacker_id) {
            nh3d_recent_attacks[i].target_id = target_id;
            return;
        }
        if (empty_slot < 0 && nh3d_recent_attacks[i].attacker_id == 0)
            empty_slot = i;
    }

    if (empty_slot < 0)
        empty_slot = (int) (attacker_id % NH3D_MONSTER_ATTACK_TRACKING_MAX);
    nh3d_recent_attacks[empty_slot].attacker_id = attacker_id;
    nh3d_recent_attacks[empty_slot].target_id = target_id;
    nh3d_recent_attacks[empty_slot].turn = monstermoves;
}

void nh3d_note_monster_attack(struct monst *, struct monst *);
void nh3d_push_monster_killer(struct monst *);
void nh3d_pop_monster_killer(void);
int nh3d_get_current_monster_killer_id(void);
void nh3d_emit_monster_killed(int, int, int, int);

void
nh3d_note_monster_attack(attacker, target)
struct monst *attacker, *target;
{
    boolean attacker_visible, target_visible;
    int attacker_id, target_id, target_x, target_y, emitted_attacker_id;

    if (!attacker || !attacker->m_id)
        return;

    attacker_visible = nh3d_monster_has_trackable_glyph(attacker);
    target_visible = target ? nh3d_monster_has_trackable_glyph(target) : TRUE;
    if (!attacker_visible && !target_visible)
        return;

    attacker_id = (int) attacker->m_id;
    if (target) {
        target_id =
            target->m_id ? (int) target->m_id : NH3D_TRACKED_MONSTER_NONE;
        target_x = target->mx;
        target_y = target->my;
    } else {
        target_id = NH3D_TRACKED_PLAYER_ID;
        target_x = u.ux;
        target_y = u.uy;
    }

    if (attacker_visible && (target == 0 || target_visible))
        nh3d_record_recent_attack(attacker->m_id, target_id);

    if (!(target == 0 || target_visible))
        return;

    emitted_attacker_id =
        attacker_visible ? attacker_id : NH3D_TRACKED_MONSTER_NONE;
    nh3d_queue_shim_notification("shim_monster_attack", emitted_attacker_id,
                                 target_id, target_x, target_y);
}

void
nh3d_push_monster_killer(killer)
struct monst *killer;
{
    int killer_id = NH3D_TRACKED_MONSTER_NONE;

    if (killer && nh3d_monster_has_trackable_glyph(killer) && killer->m_id)
        killer_id = (int) killer->m_id;

    if (nh3d_monster_killer_depth < NH3D_MONSTER_KILLER_STACK_MAX)
        nh3d_monster_killer_stack[nh3d_monster_killer_depth++] = killer_id;
}

void
nh3d_pop_monster_killer()
{
    if (nh3d_monster_killer_depth > 0)
        --nh3d_monster_killer_depth;
}

int
nh3d_get_current_monster_killer_id()
{
    return (nh3d_monster_killer_depth > 0)
               ? nh3d_monster_killer_stack[nh3d_monster_killer_depth - 1]
               : NH3D_TRACKED_MONSTER_NONE;
}

void
nh3d_emit_monster_killed(monster_id, killer_id, x, y)
int monster_id, killer_id, x, y;
{
    if (monster_id <= 0)
        return;
    nh3d_queue_shim_notification("shim_monster_killed", monster_id, killer_id,
                                 x, y);
}


/* shim_graphics_callback is the primary interface to shim graphics,
 * call this function with your declared callback function
 * and you will receive all the windowing calls
 */
#ifdef __EMSCRIPTEN__
/************
 * WASM interface
 ************/
static char *shim_callback_name = NULL;
void shim_graphics_set_callback(char *cbName);

void shim_graphics_set_callback(char *cbName) {
    if (shim_callback_name != NULL) free(shim_callback_name);
    if(cbName && strlen(cbName) > 0) {
        debugf("setting shim_callback_name: %s\n", cbName);
        shim_callback_name = strdup(cbName);
    } else {
        debugf("un-setting shim_callback_name\n");
        shim_callback_name = NULL;
    }
}
void local_callback (const char *cb_name, const char *shim_name, void *ret_ptr, const char *fmt_str, void *args);

/* A2P = Argument to Pointer */
#define A2P &
/* P2V = Pointer to Void */
#define P2V (void *)
#define DECLCB(ret_type, name, fn_args, fmt, ...) \
ret_type name fn_args; \
\
ret_type name fn_args { \
    void *args[] = { __VA_ARGS__ }; \
    ret_type ret = (ret_type) 0; \
    debugf("SHIM GRAPHICS: " #name "\n"); \
    if (!shim_callback_name) return ret; \
    local_callback(shim_callback_name, #name, (void *)&ret, fmt, args); \
    debugf("SHIM GRAPHICS: " #name " done.\n"); \
    return ret; \
}

#define VDECLCB(name, fn_args, fmt, ...) \
void name fn_args; \
\
void name fn_args { \
    void *args[] = { __VA_ARGS__ }; \
    debugf("SHIM GRAPHICS: " #name "\n"); \
    if (!shim_callback_name) return; \
    local_callback(shim_callback_name, #name, NULL, fmt, args); \
    debugf("SHIM GRAPHICS: " #name " done.\n"); \
}

#else /* !__EMSCRIPTEN__ */

/************
 * libnethack.a interface
 ************/
typedef void(*shim_callback_t)(const char *name, void *ret_ptr, const char *fmt, ...);
static shim_callback_t shim_graphics_callback = NULL;
void shim_graphics_set_callback(shim_callback_t cb);

void shim_graphics_set_callback(shim_callback_t cb) {
    shim_graphics_callback = cb;
}

#define A2P
#define P2V
#define DECLCB(ret_type, name, fn_args, fmt, ...) \
ret_type name fn_args;\
\
ret_type name fn_args { \
    ret_type ret = (ret_type) 0; \
    debugf("SHIM GRAPHICS: " #name "\n"); \
    if (!shim_graphics_callback) return ret; \
    shim_graphics_callback(#name, (void *)&ret, fmt, ## __VA_ARGS__); \
    debugf("SHIM GRAPHICS: " #name " done.\n"); \
    return ret; \
}

#define VDECLCB(name, fn_args, fmt, ...) \
void name fn_args;\
\
void name fn_args { \
    debugf("SHIM GRAPHICS: " #name "\n"); \
    if (!shim_graphics_callback) return; \
    shim_graphics_callback(#name, NULL, fmt, ## __VA_ARGS__); \
    debugf("SHIM GRAPHICS: " #name " done.\n"); \
}
#endif /* __EMSCRIPTEN__ */

static void
nh3d_queue_shim_notification(name, a, b, c, d)
const char *name;
int a, b, c, d;
{
#ifdef __EMSCRIPTEN__
    EM_ASM({
        globalThis.nethackGlobal = globalThis.nethackGlobal || {};
        if (!Array.isArray(globalThis.nethackGlobal.pendingShimNotifications)) {
            globalThis.nethackGlobal.pendingShimNotifications = [];
        }
        globalThis.nethackGlobal.pendingShimNotifications.push([
            UTF8ToString($0),
            $1,
            $2,
            $3,
            $4,
        ]);
    }, name, a, b, c, d);
#else
    if (!shim_graphics_callback)
        return;
    shim_graphics_callback(name, NULL, "viiii", a, b, c, d);
#endif
}

/*
 * Callback declarations for all window_procs functions.
 * Adapted from 3.7 to match 3.6 window_procs signatures.
 *
 * Format string: first char = return type, remaining = parameter types.
 * Types: v=void, i=int, s=string, p=pointer, c=char, b=boolean,
 *        0=1-byte int, 1=2-byte int, 2=4-byte int
 */

VDECLCB(shim_init_nhwindows,(int *argcp, char **argv), "vpp", P2V argcp, P2V argv)
/* 3.6: player_selection is void(void), not boolean(void) as in 3.7 */
VDECLCB(shim_player_selection,(void), "v")
VDECLCB(shim_askname,(void), "v")
VDECLCB(shim_get_nh_event,(void), "v")
VDECLCB(shim_exit_nhwindows,(const char *str), "vs", P2V str)
VDECLCB(shim_suspend_nhwindows,(const char *str), "vs", P2V str)
VDECLCB(shim_resume_nhwindows,(void), "v")
DECLCB(winid, shim_create_nhwindow, (int type), "ii", A2P type)
VDECLCB(shim_clear_nhwindow,(winid window), "vi", A2P window)
VDECLCB(shim_display_nhwindow,(winid window, BOOLEAN_P blocking), "vib", A2P window, A2P blocking)
VDECLCB(shim_destroy_nhwindow,(winid window), "vi", A2P window)
VDECLCB(shim_curs,(winid a, int x, int y), "viii", A2P a, A2P x, A2P y)
VDECLCB(shim_putstr,(winid w, int attr, const char *str), "viis", A2P w, A2P attr, P2V str)
VDECLCB(shim_display_file,(const char *name, BOOLEAN_P complain), "vsb", P2V name, A2P complain)
/* 3.6: start_menu takes only winid, no mbehavior */
VDECLCB(shim_start_menu,(winid window), "vi", A2P window)
/* 3.6: add_menu uses int glyph (not glyph_info*), BOOLEAN_P preselected (not unsigned int itemflags), no clr param */
VDECLCB(shim_add_menu,
    (winid window, int glyph, const ANY_P *identifier, CHAR_P ch, CHAR_P gch, int attr, const char *str, BOOLEAN_P preselected),
    "viip00isb",
    A2P window, A2P glyph, P2V identifier, A2P ch, A2P gch, A2P attr, P2V str, A2P preselected)
VDECLCB(shim_end_menu,(winid window, const char *prompt), "vis", A2P window, P2V prompt)
/* XXX: shim_select_menu menu_list is an output */
DECLCB(int, shim_select_menu,(winid window, int how, MENU_ITEM_P **menu_list), "iiip", A2P window, A2P how, P2V menu_list)
DECLCB(char, shim_message_menu,(CHAR_P let, int how, const char *mesg), "ciis", A2P let, A2P how, P2V mesg)
VDECLCB(shim_mark_synch,(void), "v")
VDECLCB(shim_wait_synch,(void), "v")
VDECLCB(shim_cliparound,(int x, int y), "vii", A2P x, A2P y)
VDECLCB(shim_update_positionbar,(char *posbar), "vs", P2V posbar)
VDECLCB(shim_raw_print,(const char *str), "vs", P2V str)
VDECLCB(shim_raw_print_bold,(const char *str), "vs", P2V str)
DECLCB(int, shim_nhgetch,(void), "i")
/* 3.6: nh_poskey uses int* (not coordxy*) */
DECLCB(int, shim_nh_poskey,(int *x, int *y, int *mod), "ippp", P2V x, P2V y, P2V mod)
VDECLCB(shim_nhbell,(void), "v")
DECLCB(int, shim_doprev_message,(void),"iv")
DECLCB(char, shim_yn_function,(const char *query, const char *resp, CHAR_P def), "css0", P2V query, P2V resp, A2P def)
VDECLCB(shim_getlin,(const char *query, char *bufp), "vsp", P2V query, P2V bufp)
DECLCB(int,shim_get_ext_cmd,(void),"iv")
VDECLCB(shim_number_pad,(int state), "vi", A2P state)
VDECLCB(shim_delay_output,(void), "v")
VDECLCB(shim_change_color,(int color, long rgb, int reverse), "viii", A2P color, A2P rgb, A2P reverse)
VDECLCB(shim_change_background,(int white_or_black), "vi", A2P white_or_black)
DECLCB(short, set_shim_font_name,(winid window_type, char *font_name),"2is", A2P window_type, P2V font_name)
DECLCB(char *,shim_get_color_string,(void),"sv")

/* 3.6-only functions */
VDECLCB(shim_start_screen,(void), "v")
VDECLCB(shim_end_screen,(void), "v")
VDECLCB(shim_outrip,(winid tmpwin, int how, time_t when), "viii", A2P tmpwin, A2P how, A2P when)

VDECLCB(shim_preference_update, (const char *pref), "vp", P2V pref)
DECLCB(char *,shim_getmsghistory, (BOOLEAN_P init), "sb", A2P init)
VDECLCB(shim_putmsghistory, (const char *msg, BOOLEAN_P restoring_msghist), "vsb", P2V msg, A2P restoring_msghist)
VDECLCB(shim_status_init, (void), "v")
VDECLCB(shim_status_enablefield,
    (int fieldidx, const char *nm, const char *fmt, BOOLEAN_P enable),
    "vippb",
    A2P fieldidx, P2V nm, P2V fmt, A2P enable)
/* XXX: the second argument to shim_status_update is sometimes an integer and sometimes a pointer */
VDECLCB(shim_status_update,
    (int fldidx, genericptr_t ptr, int chg, int percent, int color, unsigned long *colormasks),
    "vipiiip",
    A2P fldidx, P2V ptr, A2P chg, A2P percent, A2P color, P2V colormasks)

/* 3.6: print_glyph uses int glyph + int bkglyph (not glyph_info*), xchar x/y (1-byte) */
void
shim_print_glyph(w, x, y, glyph, bkglyph)
winid w;
XCHAR_P x, y;
int glyph, bkglyph;
{
    int monster_id = nh3d_monster_id_from_tile(x, y, glyph);
    int attacking_target_id = nh3d_get_recent_attack_target_id(monster_id);

    debugf("SHIM GRAPHICS: shim_print_glyph\n");
#ifdef __EMSCRIPTEN__
    if (shim_callback_name) {
        void *args[] = { A2P w, A2P x, A2P y, A2P glyph, A2P bkglyph,
                         A2P monster_id, A2P attacking_target_id };
        local_callback(shim_callback_name, "shim_print_glyph", NULL,
                       "vi00iiii", args);
    }
#else
    if (shim_graphics_callback)
        shim_graphics_callback("shim_print_glyph", NULL, "vi00iiii", w, x, y,
                               glyph, bkglyph, monster_id,
                               attacking_target_id);
#endif
    debugf("SHIM GRAPHICS: shim_print_glyph done.\n");
}

#ifdef __EMSCRIPTEN__
/* update_inventory is called by the game engine during command processing,
 * while the shim callback is still on the Asyncify stack. Calling
 * local_callback here would cause Asyncify reentrancy, which crashes.
 * Instead, set a JavaScript flag that local_callback checks on its next
 * entry and delivers as a non-blocking notification. */
void shim_update_inventory() {
    EM_ASM({
        globalThis.nethackGlobal = globalThis.nethackGlobal || {};
        globalThis.nethackGlobal.pendingInventoryUpdate = true;
    });
}
#else /* !__EMSCRIPTEN__ */
VDECLCB(shim_update_inventory,(void), "v")
#endif

/* Interface definition used in windows.c
 * Must match the order in 3.6 struct window_procs (include/winprocs.h)
 */
struct window_procs shim_procs = {
    "shim",
    (0
     | WC_ASCII_MAP
     | WC_MOUSE_SUPPORT
     | WC_COLOR | WC_HILITE_PET | WC_INVERSE | WC_EIGHT_BIT_IN),
    (0
#if defined(SELECTSAVED)
     | WC2_SELECTSAVED
#endif
#if defined(STATUS_HILITES)
     | WC2_HILITE_STATUS | WC2_HITPOINTBAR | WC2_FLUSH_STATUS
     | WC2_RESET_STATUS
#endif
     | WC2_DARKGRAY | WC2_SUPPRESS_HIST | WC2_STATUSLINES),
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},   /* color availability */
    shim_init_nhwindows,
    shim_player_selection,
    shim_askname,
    shim_get_nh_event,
    shim_exit_nhwindows,
    shim_suspend_nhwindows,
    shim_resume_nhwindows,
    shim_create_nhwindow,
    shim_clear_nhwindow,
    shim_display_nhwindow,
    shim_destroy_nhwindow,
    shim_curs,
    shim_putstr,
    genl_putmixed,          /* putmixed */
    shim_display_file,
    shim_start_menu,
    shim_add_menu,
    shim_end_menu,
    shim_select_menu,
    shim_message_menu,
    shim_update_inventory,
    shim_mark_synch,
    shim_wait_synch,
#ifdef CLIPPING
    shim_cliparound,
#endif
#ifdef POSITIONBAR
    shim_update_positionbar,
#endif
    shim_print_glyph,
    shim_raw_print,
    shim_raw_print_bold,
    shim_nhgetch,
    shim_nh_poskey,
    shim_nhbell,
    shim_doprev_message,
    shim_yn_function,
    shim_getlin,
    shim_get_ext_cmd,
    shim_number_pad,
    shim_delay_output,
#ifdef CHANGE_COLOR
    shim_change_color,
#ifdef MAC
    shim_change_background,
    set_shim_font_name,
#endif
    shim_get_color_string,
#endif
    /* 3.6-only: start_screen, end_screen */
    shim_start_screen,
    shim_end_screen,
    shim_outrip,
    shim_preference_update,
    shim_getmsghistory,
    shim_putmsghistory,
    shim_status_init,
    genl_status_finish,
    genl_status_enablefield,
#ifdef STATUS_HILITES
    shim_status_update,
#else
    genl_status_update,
#endif
    genl_can_suspend_no,
};

#ifdef __EMSCRIPTEN__
/* convert the C callback to a JavaScript callback */
EM_JS(void, local_callback, (const char *cb_name, const char *shim_name, void *ret_ptr, const char *fmt_str, void *args), {
    /* Dispatch any inventory update that was queued since the last callback.
     * This runs synchronously before handleSleep, so there is no Asyncify
     * stack to re-enter. The user callback's returned Promise is intentionally
     * not awaited -- the notification will resolve after handleSleep suspends. */
    if (globalThis.nethackGlobal && globalThis.nethackGlobal.pendingInventoryUpdate) {
        globalThis.nethackGlobal.pendingInventoryUpdate = false;
        let pendingCbName = UTF8ToString(cb_name);
        let pendingCb = globalThis[pendingCbName];
        if (pendingCb) {
            pendingCb.call(null, 'shim_update_inventory');
        }
    }
    if (globalThis.nethackGlobal
        && Array.isArray(globalThis.nethackGlobal.pendingShimNotifications)
        && globalThis.nethackGlobal.pendingShimNotifications.length > 0) {
        let pendingCbName = UTF8ToString(cb_name);
        let pendingCb = globalThis[pendingCbName];
        if (pendingCb) {
            const pendingNotifications =
                globalThis.nethackGlobal.pendingShimNotifications.splice(0);
            for (const pendingArgs of pendingNotifications) {
                pendingCb.call(null, ...pendingArgs);
            }
        }
    }

    // Asyncify.handleAsync() is the more logical choice here; however, the stack unrolling in Asyncify is performed by
    // function call analysis during compilation. Since we are using an indirect callback (cb_name), it can't predict the stack
    // unrolling and it crashes. Thus we use Asyncify.handleSleep() and wakeUp() to make sure that async doesn't break
    // Asyncify. For details, see: https://emscripten.org/docs/porting/asyncify.html#optimizing
    Asyncify.handleSleep(wakeUp => {
        // convert callback arguments to proper JavaScript variadic arguments
        let name = UTF8ToString(shim_name);
        let fmt = UTF8ToString(fmt_str);
        let cbName = UTF8ToString(cb_name);
        // console.log("local_callback:", cbName, fmt, name);

        // get pointer / type conversion helpers
        let getPointerValue = globalThis.nethackGlobal.helpers.getPointerValue;
        let setPointerValue = globalThis.nethackGlobal.helpers.setPointerValue;

        reentryGuardEnter(name);

        let argTypes = fmt.split("");
        let retType = argTypes.shift();

        // build array of JavaScript args from WASM parameters
        let jsArgs = [];
        for (let i = 0; i < argTypes.length; i++) {
            let ptr = args + (4*i);
            let val = getArg(name, ptr, argTypes[i]);
            jsArgs.push(val);
        }

        // do the callback
        let userCallback = globalThis[cbName];
        userCallback.call(this, name, ... jsArgs).then((retVal) => {
            // save the return value
            setPointerValue(name, ret_ptr, retType, retVal);
            reentryGuardExit();
            try {
                wakeUp();
            } catch (e) {
                console.error("Asyncify wakeUp failed:", e);
            }
        });

        function getArg(name, ptr, type) {
            return (type === "p") ? getValue(ptr, "*") : getPointerValue(name, getValue(ptr, "*"), type);
        }

        function reentryGuardEnter(name) {
            globalThis.nethackGlobal = globalThis.nethackGlobal || {};
            if(globalThis.nethackGlobal.shimFunctionRunning) {
                console.error(`'${name}' attempting second call to 'local_callback' before '${globalThis.nethackGlobal.shimFunctionRunning}' has finished, will crash emscripten Asyncify. For details see: emscripten.org/docs/porting/asyncify.html#reentrancy`);
            }
            globalThis.nethackGlobal.shimFunctionRunning = name;
        }

        function reentryGuardExit() {
            globalThis.nethackGlobal.shimFunctionRunning = null;
        }
    });
})
#endif /* __EMSCRIPTEN__ */

#endif /* SHIM_GRAPHICS */
