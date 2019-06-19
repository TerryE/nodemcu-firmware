/*
** $Id: lua.c,v 1.160.1.2 2007/12/28 15:32:23 roberto Exp $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/

// #include "c_signal.h"
#include "c_stdio.h"
#include "c_stdlib.h"
#include "c_string.h"
#include "user_interface.h"
#include "user_version.h"
#include "driver/input.h"
#include "driver/uart.h"
#include "platform.h"

#define lua_c

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "legc.h"
#include "lflash.h"
#include "os_type.h"

lua_State *globalL = NULL;

static void l_message (const char *msg) {
  luai_writestringerror("%s\n", msg);
}

static int report (lua_State *L, int status) {
  if (status && !lua_isnil(L, -1)) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL) msg = "(error object is not a string)";
    l_message(msg);
    lua_pop(L, 1);
  }
  return status;
}

static void l_print(lua_State *L, int n) {
  lua_getglobal(L, "print");
  lua_insert(L, -n-1);
  if (lua_pcall(L, n, 0, 0) != 0)
    l_message(lua_pushfstring(L, "error calling " LUA_QL("print") " (%s)",
                              lua_tostring(L, -1)));
}

static int traceback (lua_State *L) {
  if (lua_isstring(L, 1)) {
    lua_getfield(L, LUA_GLOBALSINDEX, "debug");
    if (lua_isrotable(L, -1) || lua_istable(L, -1)) {
      lua_getfield(L, -1, "traceback");
      if (lua_isfunction(L, -1) || lua_islightfunction(L, -1)) {
        lua_pushvalue(L, 1);    /* pass error message */
        lua_pushinteger(L, 2);  /* skip this function and traceback */
        lua_call(L, 2, 1);      /* call debug.traceback */
      }
    }
  }
  lua_settop(L, 1);
  return 1;
}

static int docall (lua_State *L, int narg, int clear) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, traceback);  /* push traceback function */
  lua_insert(L, base);  /* put it under chunk and args */
  status = lua_pcall(L, narg, (clear ? 0 : LUA_MULTRET), base);
  lua_remove(L, base);  /* remove traceback function */
  /* force a complete garbage collection in case of errors */
  if (status != 0) lua_gc(L, LUA_GCCOLLECT, 0);
  return status;
}

static void print_version (lua_State *L) {
  lua_pushliteral (L, "\n" NODE_VERSION " build " BUILD_DATE " powered by " LUA_RELEASE " on SDK ");
  lua_pushstring (L, SDK_VERSION);
  lua_concat (L, 2);
  const char *msg = lua_tostring (L, -1);
  l_message (msg);
  lua_pop (L, 1);
}

static int dofsfile (lua_State *L, const char *name) {
  int status = luaL_loadfsfile(L, name) || docall(L, 0, 1);
  return report(L, status);
}

static int dostring (lua_State *L, const char *s, const char *name) {
  int status = luaL_loadbuffer(L, s, c_strlen(s), name) || docall(L, 0, 1);
  return report(L, status);
}

static const char *get_prompt (lua_State *L, int firstline) {
  const char *p;
  lua_getfield(L, LUA_GLOBALSINDEX, firstline ? "_PROMPT" : "_PROMPT2");
  p = lua_tostring(L, -1);
  if (p == NULL) p = (firstline ? LUA_PROMPT : LUA_PROMPT2);
  lua_pop(L, 1);  /* remove global */
  return p;
}

static int incomplete (lua_State *L, int status) {
  if (status == LUA_ERRSYNTAX) {
    size_t lmsg;
    const char *msg = lua_tolstring(L, -1, &lmsg);
    if (!c_strcmp(msg+lmsg-sizeof(LUA_QL("<eof>"))+1, LUA_QL("<eof>"))) {
      lua_pop(L, 1);
      return 1;
    }
  }
  return 0;
}

#ifndef LUA_INIT_STRING
#define LUA_INIT_STRING "@init.lua"
#endif

static int handle_luainit (lua_State *L) {
  const char *init = LUA_INIT_STRING;
  if (init[0] == '@')
    return dofsfile(L, init+1);
  else
    return dostring(L, init, LUA_INIT);
}
static int pmain (lua_State *L) {
  globalL = L;
  lua_gc(L, LUA_GCSTOP, 0);  /* stop collector during initialization */
  luaL_openlibs(L);  /* open libraries */
  lua_gc(L, LUA_GCRESTART, 0);

  lua_pushliteral(L, "");
  lua_queueline(L, 1);

  print_version(L);
  lua_pushinteger(L,handle_luainit(L));
  return 1;
}

extern void dbg_printf(const char *fmt, ...) __attribute__ ((format (printf, 1, 2))); /**DEBUG**/

int lua_main (void) {

#if defined(NODE_DEBUG) && defined(DEVELOPMENT_USE_GDB) && \
    defined(DEVELOPMENT_BREAK_ON_STARTUP_PIN) && DEVELOPMENT_BREAK_ON_STARTUP_PIN > 0
  platform_gpio_mode( DEVELOPMENT_BREAK_ON_STARTUP_PIN, PLATFORM_GPIO_INPUT, PLATFORM_GPIO_PULLUP );
  lua_assert(platform_gpio_read(DEVELOPMENT_BREAK_ON_STARTUP_PIN));  // Break if pin pulled low
#endif
  int status;
  lua_State *L = lua_open();  /* create state */
  if (L == NULL) {
    l_message("cannot create state: not enough memory");
    return EXIT_FAILURE;
  }
  lua_pushcfunction(L, &pmain);  /* Call 'pmain' in protected mode */
  status = lua_pcall(L, 0, 1, 0 );
  report(L, status);

  input_setup(LUA_MAXINPUT, get_prompt(L, 1));

  NODE_DBG("Heap size:%d.\n",system_get_free_heap_size());
  legc_set_mode( L, EGC_ALWAYS, 4096 );
  return (status) ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void del_entry(lua_State *L, int ndx, int i) {                //  [-0, +0, -]
  lua_pushvalue(L, ndx);
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "remove");
  lua_remove(L, -2);   /* dump table reference */
  lua_insert(L, -2);   /* reorder to table.remove, <table> */
  lua_pushinteger(L, i);
  lua_call(L, 2, 0);
}

static struct {
  int lineQ[2];
  bool multiline[2];
} Qstate = { {LUA_NOREF, LUA_NOREF}, {false, false} };

/*
** lua_dojob can process one of two input streams in the lineQ[prio] where prio = 0 or 1 is the
** call argument.  The job is in one of two modes:
** -  First_line (and possibly singleton) in which case the Q only needs one entry
**
** -  Multi-line in which case the Q only needs at least 2 entries, with the first being an
**    aggregation of previous compiled lines.
*/
static int lua_dojob (lua_State *L) {
  size_t l;
  int status;
  const char *prompt;
  int prio      = lua_tointeger(L, 1) ? 1 : 0;
  int multiline = Qstate.multiline[prio];
  int lineQ     = Qstate.lineQ[prio];
  lua_settop(L, 0);

  lua_rawgeti(L, LUA_REGISTRYINDEX, lineQ);
  int n = lua_istable(L, -1) ? lua_objlen(L, -1) : -1;

  /* If the lineQ slot doesn't point to a non-empty line Q array then return */
  if (n <= 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, lineQ);
    Qstate.lineQ[prio] = LUA_NOREF;
    Qstate.multiline[prio] = false;
    return 0;
  }

  if (n == 1 && multiline)
    return 0;

  lua_rawgeti(L, 1, 1);                              /* push line 1 onto stack */
  if (multiline) {
    lua_pushliteral(L, "\n");                            /* push CR onto stack */
    lua_rawgeti(L, 1, 2);                            /* push line 2 onto stack */
  }

  const char* b = lua_tolstring(L, -1, &l);
  if (l > 0 && b[l-1] == '\n')  {  /* If line ends with newline then remove it */
    lua_pushlstring(L, b, l-1);
    lua_remove(L, -2);
    b = lua_tolstring(L, -1, &l);
  }

  if (multiline) {
    lua_concat(L, 3);
    del_entry(L, 1, 2);          /* remove previous (aggregate) line from Q[2] */
    n--;
  } else if (b[0] == '=') {                 /* If firstline then s/^=/return / */
    lua_pushfstring(L, "return %s", b+1);
    lua_remove(L, 2);
  }

  /* Try to compile ToS and check for incomplete line */
  int top = lua_gettop(L);
  status = luaL_loadbuffer(L, lua_tostring(L, -1), lua_strlen(L, -1), "=stdin");

  if (incomplete(L, status)) {
    lua_rawseti(L, 1, 1);                              /* put ToS back in Q[1] */
    multiline = 1;
  } else {                           /* compile finished OK or with hard error */
    lua_remove(L, top--);        /* remove source line because it isn't needed */
    del_entry(L, 1, 1);                        /* remove source line from Q[1] */
    n--;
    if (status == 0) {
      status = docall(L, 0, 0);          /* execute the code if it compiled OK */
    }

    if (status && !lua_isnil(L, -1))
      l_print(L, 1);
    if (status == 0 && lua_gettop(L) > top)            /* any result to print? */
      l_print(L, lua_gettop(L)-top);
    multiline = 0;
    lua_settop(L, top);
    if (status != 0) lua_gc(L, LUA_GCCOLLECT, 0);
  }

  prompt = get_prompt(L, multiline ? 0 : 1);
  if (prio)

    input_setprompt(prompt);     /* only set input prompt for interactive lineQ */
  c_puts(prompt);

  if (n == 0) {                /* If empty clear down Q and wait for next input */

    luaL_unref(L, LUA_REGISTRYINDEX, lineQ);
    lineQ = LUA_NOREF;

    input_process_arm();

  } else if (n == 1 && multiline) {       /* If 1 multiline wait for next input */
    input_process_arm();
  } else {     /* otherwise repost dojob task to compile an execute remaining Q */
    lua_pushlightfunction(L, &lua_dojob);
    lua_posttask(L, prio);

  }
  Qstate.multiline[prio] = multiline;
  Qstate.lineQ[prio] = lineQ;
  return 0;
}

/*
** The Lua interpreter is event-driven and task-oriented in NodeMCU rather than based on
** a readline poll loop as in the standard implementation.  Input lines can come from one
** of two sources:  the application can "push" lines for the interpreter to compile and
** execute, or they can come from the UART.  To minimise application blocking, the lines
** are queued when received, and a Lua interpreter task is posted at low priority to do the
** compilation and execution, with one task execution per line input.
**
** Because the lines can be queued from multiple independent sources (the UART and the node
** API), the Lua stack can't use, and so a registry slot is used to hold each queue in a
** Lua array.  In true interactive use a single line will be queued then immediately scheduled
** for compilation, but bulk calls (e.g. when a function is pasted into a telnet session), the
** queue might grow to multiple entries.
*/
void lua_queueline(lua_State *L, int queue) {                                  // [-1, +0, -]
  int n = 0;
  if (Qstate.lineQ[queue] == LUA_NOREF) {
    // if slot linbuf is NOREF then allocate array and store it in the lineQ slot;
    lua_createtable (L, 1, 0);
    lua_pushvalue(L, -1);
    Qstate.lineQ[queue] = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    lua_rawgeti(L, LUA_REGISTRYINDEX, Qstate.lineQ[queue]);
    n = lua_objlen(L, -1);
  }
  lua_insert(L, -2);         // move table below new string and then add to table
  lua_rawseti(L, -2, ++n);
  lua_pop(L, 1);

  if (n == (Qstate.multiline[queue] ? 2 : 1)) {
    lua_pushlightfunction(L, &lua_dojob);
    lua_posttask(L, queue);  // We have two Qs and post Q[0] at low priority, Q[1] at medium
  }
}

// Wrapper for call from UART driver
void lua_input_string (const char *line, int len) {
  lua_State *L = globalL;
  lua_pushlstring(L, line, len);
  lua_queueline(L, 1);
}

// Task callback handler
static void do_task (task_param_t task_fn_ref, uint8_t prio) {
  lua_State* L = lua_getstate();
  lua_rawgeti(L, LUA_REGISTRYINDEX, (int)task_fn_ref);
  luaL_unref(L, LUA_REGISTRYINDEX, (int)task_fn_ref);
  if (!ttisanyfunction(L->top-1) || prio < 0|| prio > 2)
    luaL_error(L, "invalid posk task");

  lua_pushinteger(L, prio);
  lua_call(L, 1, 0);
}

// Schedule a lua function for task execution
LUA_API void lua_posttask( lua_State* L, int prio ) {                       // [-1, +0, -]
  static task_handle_t task_handle;

  if (!ttisanyfunction(L->top-1) || prio < 0|| prio > 2)
    luaL_error(L, "invalid posk task");

  if (!task_handle)  // bind the task handle to do_node_task on 1st call
    task_handle = task_get_id(do_task);

  int task_fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  if(!task_post(prio, task_handle, (task_param_t)task_fn_ref)) {
    luaL_unref(L, LUA_REGISTRYINDEX, task_fn_ref);
    luaL_error(L, "Task queue overflow. Task not posted");
  }
}
