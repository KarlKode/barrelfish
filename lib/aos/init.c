/**
 * \file
 * \brief Barrelfish library initialization.
 */

/*
 * Copyright (c) 2007-2016, ETH Zurich.
 * Copyright (c) 2014, HP Labs.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, CAB F.78, Universitaetstr. 6, CH-8092 Zurich,
 * Attn: Systems Group.
 */

#include <stdio.h>
#include <aos/aos.h>
#include <aos/aos_rpc.h>
#include <aos/dispatch.h>
#include <aos/curdispatcher_arch.h>
#include <aos/dispatcher_arch.h>
#include <barrelfish_kpi/dispatcher_shared.h>
#include <aos/morecore.h>
#include <aos/paging.h>
#include <barrelfish_kpi/domain_params.h>
#include "threads_priv.h"
#include "init.h"

/// Are we the init domain (and thus need to take some special paths)?
static bool init_domain;

extern size_t (*_libc_terminal_read_func)(char *, size_t);
extern size_t (*_libc_terminal_write_func)(const char *, size_t);
extern void (*_libc_exit_func)(int);
extern void (*_libc_assert_func)(const char *, const char *, const char *, int);

void libc_exit(int);

void libc_exit(int status)
{
    // Use spawnd if spawned through spawnd
    if(disp_get_domain_id() == 0) {
        errval_t err = cap_revoke(cap_dispatcher);
        if (err_is_fail(err)) {
            sys_print("revoking dispatcher failed in _Exit, spinning!", 100);
            while (1) {}
        }
        err = cap_delete(cap_dispatcher);
        sys_print("deleting dispatcher failed in _Exit, spinning!", 100);

        // XXX: Leak all other domain allocations
    } else {
        debug_printf("libc_exit NYI!\n");
    }

    thread_exit(status);
    // If we're not dead by now, we wait
    while (1) {}
}

static void libc_assert(const char *expression, const char *file,
                        const char *function, int line)
{
    char buf[512];
    size_t len;

    /* Formatting as per suggestion in C99 spec 7.2.1.1 */
    len = snprintf(buf, sizeof(buf), "Assertion failed on core %d in %.*s: %s,"
                   " function %s, file %s, line %d.\n",
                   disp_get_core_id(), DISP_NAME_LEN,
                   disp_name(), expression, function, file, line);
    sys_print(buf, len < sizeof(buf) ? len : sizeof(buf));
}

static size_t syscall_terminal_write(const char *buf, size_t len)
{
    if (len) {
        return sys_print(buf, len);
    }
    return 0;
}

static size_t aos_terminal_write(const char* buf, size_t len)
{
    if (len > 0) {
        debug_printf("init.c: calling aos_rpc_send_string\n");
        return aos_rpc_send_string(get_init_rpc(), buf);
    }
    return 0;
}

static size_t dummy_terminal_read(char *buf, size_t len)
{
    debug_printf("terminal read NYI! returning %d characters read\n", len);
    return len;
}

/* Set libc function pointers */
void barrelfish_libc_glue_init(void)
{
    // XXX: FIXME: Check whether we can use the proper kernel serial, and
    // what we need for that
    // TODO: change these to use the user-space serial driver if possible
    _libc_terminal_read_func = dummy_terminal_read;
    _libc_terminal_write_func = init_domain ? syscall_terminal_write : aos_terminal_write;
    _libc_exit_func = libc_exit;
    _libc_assert_func = libc_assert;
    /* morecore func is setup by morecore_init() */

    // XXX: set a static buffer for stdout
    // this avoids an implicit call to malloc() on the first printf
    static char buf[BUFSIZ];
    setvbuf(stdout, buf, _IOLBF, sizeof(buf));
    static char ebuf[BUFSIZ];
    setvbuf(stderr, ebuf, _IOLBF, sizeof(buf));
}

/** \brief Initialise libbarrelfish.
 *
 * This runs on a thread in every domain, after the dispatcher is setup but
 * before main() runs.
 */
errval_t barrelfish_init_onthread(struct spawn_domain_params *params)
{
    errval_t err;

    // do we have an environment?
    if (params != NULL && params->envp[0] != NULL) {
        extern char **environ;
        environ = params->envp;
    }

    // Init default waitset for this dispatcher
    struct waitset *default_ws = get_default_waitset();
    waitset_init(default_ws);

    // Initialize ram_alloc state
    ram_alloc_init();
    /* All domains use smallcn to initialize */
    err = ram_alloc_set(init_domain ? ram_alloc_fixed : NULL);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_RAM_ALLOC_SET);
    }

    err = morecore_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_MORECORE_INIT);
    }

    err = paging_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_VSPACE_INIT);
    }

    err = slot_alloc_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SLOT_ALLOC_INIT);
    }

    lmp_endpoint_init();

    // init domains only get partial init
    if (init_domain) {
        return SYS_ERR_OK;
    }

    // Initialize RPC channel.
    struct aos_rpc* rpc = (struct aos_rpc*) malloc(sizeof(struct aos_rpc));
    CHECK("init.c#barrelfish_init_onthread: aos_rpc_init",
            aos_rpc_init(rpc, get_default_waitset()));
    // Set domain init rpc.
    set_init_rpc(rpc);
    debug_printf("init.c: successfully setup connection with init\n");

    // struct capref frame;
    // size_t retsize;
    // CHECK("init.c#barrelfish_init_onthread: aos_rpc_get_ram_cap",
    //         aos_rpc_get_ram_cap(rpc, 16 * 1024 * 1024, &frame, &retsize));
    // debug_printf("init.c: Client asked for %u memory, was given %u\n",
    //     16 * 1024 * 1024, retsize);

    // void* buf;
    // err = paging_map_frame_attr(get_current_paging_state(),
    //     &buf, retsize, frame,
    //     VREGION_FLAGS_READ_WRITE, NULL, NULL);
    // if (err_is_fail(err)) {
    //     DEBUG_ERR(err, "PANIC MAPPING 16 MB FRAME IN CHILD");
    // }

    // debug_printf("init.c: testing memory @ %p\n", buf);
    // char* cbuf = (char*)buf;
    // *cbuf = 'J';
    // sys_debug_flush_cache();
    // debug_printf("%c\n", *cbuf);

    // cbuf += 4 * 1024 * 1024;
    // *cbuf = 'K';
    // sys_debug_flush_cache();
    // debug_printf("%c\n", *cbuf);

    // cbuf += 4 * 1024 * 1024;
    // *cbuf = 'L';
    // sys_debug_flush_cache();
    // debug_printf("%c\n", *cbuf);

    // cbuf += 4 * 1024 * 1024;
    // *cbuf = 'M';
    // sys_debug_flush_cache();
    // debug_printf("%c\n", *cbuf);

    // // Ask for more memory -- attempt to break 64 MB limitation.
    // CHECK("init.c#barrelfish_init_onthread: aos_rpc_get_ram_cap",
    //         aos_rpc_get_ram_cap(rpc, 16 * 1024 * 1024, &frame, &retsize));
    // debug_printf("init.c: Client asked for %u memory, was given %u\n",
    //     16 * 1024 * 1024, retsize);
    // CHECK("init.c#barrelfish_init_onthread: aos_rpc_get_ram_cap",
    //         aos_rpc_get_ram_cap(rpc, 16 * 1024 * 1024, &frame, &retsize));
    // debug_printf("init.c: Client asked for %u memory, was given %u\n",
    //     16 * 1024 * 1024, retsize);
    // CHECK("init.c#barrelfish_init_onthread: aos_rpc_get_ram_cap",
    //         aos_rpc_get_ram_cap(rpc, 24 * 1024 * 1024, &frame, &retsize));
    // debug_printf("init.c: Client asked for %u memory, was given %u\n",
    //     24 * 1024 * 1024, retsize);

    // // RPC send number.
    // CHECK("init.c#barrelfish_init_onthread: aos_rpc_send_number",
    //         aos_rpc_send_number(rpc, 1337));

    // // RPC putchar.
    // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    //         aos_rpc_serial_putchar(rpc, 'T'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'H'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'I'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'S'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, ' '));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'I'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'S'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, ' '));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'A'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, ' '));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'F'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, '*'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, '*'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, '*'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'I'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'N'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'G'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, ' '));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'R'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'P'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, 'C'));
    // // CHECK("init.c#barrelfish_init_onthread: aos_rpc_serial_putchar",
    // //         aos_rpc_serial_putchar(rpc, '\n'));

    // // Stringz.
    // CHECK("init.c#barrelfish_init_onthread: aos_rpc_send_string",
    //         aos_rpc_send_string(rpc, "this is such a long it makes me sad\n"));

    // right now we don't have the nameservice & don't need the terminal
    // and domain spanning, so we return here
    return SYS_ERR_OK;
}


/**
 *  \brief Initialise libbarrelfish, while disabled.
 *
 * This runs on the dispatcher's stack, while disabled, before the dispatcher is
 * setup. We can't call anything that needs to be enabled (ie. cap invocations)
 * or uses threads. This is called from crt0.
 */
void barrelfish_init_disabled(dispatcher_handle_t handle, bool init_dom_arg);
void barrelfish_init_disabled(dispatcher_handle_t handle, bool init_dom_arg)
{
    init_domain = init_dom_arg;
    disp_init_disabled(handle);
    thread_init_disabled(handle, init_dom_arg);
}
