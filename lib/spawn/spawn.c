#include <aos/aos.h>
#include <spawn/spawn.h>

#include <elf/elf.h>
#include <aos/dispatcher_arch.h>
#include <barrelfish_kpi/paging_arm_v7.h>
#include <barrelfish_kpi/domain_params.h>
#include <spawn/multiboot.h>

#define DPRINT(fmt, args...)  debug_printf("spawn: " fmt "\n", args)

#define CHECK(where, err)  \
        if (err_is_fail(err)) {  \
            DPRINT("ERROR %s: %s", where, err_getstring(err));  \
            return err;  \
        }

#define CHECK_COND(what, cond, err)  \
        if (!(cond)) {  \
            DPRINT("FAIL %s: %s (%s)\n", what, err_getstring(err), #cond);  \
            return err;  \
        }

#define STATIC_ARRAY_LENGTH(x) (sizeof(x) / sizeof((x)[0]))


extern struct bootinfo *bi;

void setup_cspace(struct spawninfo *si) {
    errval_t err;
    err = cnode_create_l1(&(si->l1_cap), &(si->l1_cnoderef));
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }

    // Create TASKCN
    err = cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_TASKCN, &(si->taskcn));
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }

    // Create SLOT PAGECN
    err = cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_PAGECN, &(si->pagecn));
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }

    // Create SLOT BASE PAGE CN
    err = cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_BASE_PAGE_CN, &(si->base_pagecn));
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }

    // Create SLOT ALLOC 0
    err = cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_SLOT_ALLOC0, &(si->alloc0));
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }

    // Create SLOT ALLOC 1
    err = cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_SLOT_ALLOC1, &(si->alloc1));
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }

    // Create SLOT ALLOC 2
    err = cnode_create_foreign_l2(si->l1_cap, ROOTCN_SLOT_SLOT_ALLOC2, &(si->alloc2));
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }

    // Create SLOT DISPATCHER
    si->dispatcher.cnode = si->taskcn;
    si->dispatcher.slot = TASKCN_SLOT_DISPATCHER;
    err = dispatcher_create(si->dispatcher);
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }
    // err = cap_copy(si->dispatcher, cap_dispatcher);
    // if(err_is_fail(err)) {
    //     printf("%s\n", err_getstring(err));
    // }

    // Create SLOT ROOTCN
    si->rootcn.cnode = si->taskcn;
    si->rootcn.slot = TASKCN_SLOT_ROOTCN;

    // Copy the cap from l1 to rootcn
    err = cap_copy(si->rootcn, si->l1_cap);
    if(err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }
    // err = cap_copy(si->rootcn, cap_root);
    // if(err_is_fail(err)) {
    //     printf("%s\n", err_getstring(err));
    // }

    // Create SLOT DISPFRAME
    si->dispframe.cnode = si->taskcn;
    si->dispframe.slot = TASKCN_SLOT_DISPFRAME;

    // err = cap_copy(si->dispframe, cap_dispframe);
    // if(err_is_fail(err)) {
    //     printf("%s\n", err_getstring(err));
    // }

    // Create SLOT ARGSPG
    si->argspg.cnode = si->taskcn;
    si->argspg.slot = TASKCN_SLOT_ARGSPAGE;

    // err = cap_copy(si->argspg, cap_argcn);
    // if(err_is_fail(err)) {
    //     printf("%s\n", err_getstring(err));
    // }

    cap_retype(si->selfep, si->dispatcher, 0, ObjType_EndPoint, 0, 1);

}

void setup_vspace(struct spawninfo *si) {
    // 1. Create slot allocator for child vspace.
    size_t bufsize = SINGLE_SLOT_ALLOC_BUFLEN(L2_CNODE_SLOTS);
    void *buf = malloc(bufsize);
    assert(buf != NULL);

    struct capref pagecn_cap = {
        .cnode = si->l1_cnoderef,
        .slot = ROOTCN_SLOT_PAGECN
    };
    errval_t err = single_slot_alloc_init_raw(
            &si->ssa,
            pagecn_cap,
            si->pagecn,
            L2_CNODE_SLOTS,
            buf,
            bufsize);
    if (err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }
    si->ssa.head->slot = 1;
    --si->ssa.head->space;

    // 2. Create child paging state.
    // TODO: Where should the pdir capref come from?
    struct capref pdir;
    err = paging_init_state(&si->pg_state, 0, pdir, &si->ssa.a);
    if (err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
        return;
    }

    // 3. Create child L1 pagetable.
    si->pg_state.l1_pagetable.cnode = si->pagecn;
    si->pg_state.l1_pagetable.slot = 0;
    err = vnode_create(si->pg_state.l1_pagetable, ObjType_VNode_ARM_l1);
    if (err_is_fail(err)) {
        printf("%s\n", err_getstring(err));
    }
}


// Stuffs the ELF in the memory, preparing it for execution:
// - mapping sections where they need to be
// - dealing with the GOT
errval_t load_elf_into_memory(struct spawninfo *si, lvaddr_t base, size_t size) {
    // stuff the sections into memory
    CHECK("loading ELF (lib fn)", elf_load(EM_ARM, elf_section_allocate, (void*)&si->pg_state,
                    base, size, &si->entry_point));

    // find the GOT -- this will be needed by the dispatcher
    struct Elf32_Shdr *got_shdr = elf32_find_section_header_name(base, size, ".got");
    si->got_ubase = (genvaddr_t)got_shdr->sh_addr;

    return SYS_ERR_OK;
}

// Handles ELF sections.
// I infer that...
//  - base is the requested virtual address -- "please make me appear here in
//    the child process's vspace", says the section header
//  - size is size
//  - flags is
//  - dest is the address in this vspace where we need to copy things (so that they appear there)
//  - state should be just paging_state (which will be in spawninfo)
errval_t elf_section_allocate(void *state_void, genvaddr_t base, size_t size,
                                     uint32_t flags, void **ret) {
    if (size == 0) return SYS_ERR_OK;
    struct paging_state *state = (struct paging_state *) state_void;
    size_t retsize;
    struct capref my_frame;
    printf("allocating section of size %u\n", size);
    CHECK("allocating slot for my frame cap", slot_alloc(&my_frame));
    CHECK("allocating frame for function", frame_alloc(&my_frame, size, &retsize));
    printf("BEFORE paging_map_frame\n");
    CHECK("mapping ELF section to my vspace",
          paging_map_frame(get_current_paging_state(), ret, retsize, my_frame, NULL, NULL));

    struct capref child_frame;
    CHECK("allocating slot for child's frame cap",
          state->slot_alloc->alloc(state->slot_alloc, &child_frame));
    printf("child_frame.slot: %u\n", child_frame.slot);
    CHECK("copying section frame cap", cap_copy(child_frame, my_frame));
    errval_t err = paging_map_fixed_attr(state, (lvaddr_t)base, child_frame, retsize, flags);
    printf("AFTER paging_map_fixed_attr\n");
    CHECK("mapping section in child\n", err);
    return SYS_ERR_OK;
}

errval_t setup_dispatcher(struct spawninfo *si) {
    size_t retsize;
    CHECK("allocating frame for dispatcher",
          frame_alloc(&si->dispframe, 1<<DISPATCHER_FRAME_BITS, &retsize));

    struct capref my_dispframe;
    CHECK("allocating slot for dispatcher frame cap", slot_alloc(&my_dispframe));
    CHECK("copying dispatcher frame cap", cap_copy(my_dispframe, si->dispframe));
    assert(1<<DISPATCHER_FRAME_BITS <= retsize);

    void *disp_addr_in_me;
    CHECK("mapping dispatcher frame into my vspace",
          paging_map_frame(get_current_paging_state(), &disp_addr_in_me,
                           retsize, my_dispframe, NULL, NULL));
    si->disp_handle = (dispatcher_handle_t)disp_addr_in_me;

    void *disp_addr_in_child;
    CHECK("mapping dispatcher frame into child's vspace",
          paging_map_frame(&si->pg_state, &disp_addr_in_child,
                           retsize, si->dispframe, NULL, NULL));

    struct dispatcher_shared_generic *disp = get_dispatcher_shared_generic(si->disp_handle);
    struct dispatcher_generic *disp_gen = get_dispatcher_generic(si->disp_handle);
    struct dispatcher_shared_arm *disp_arm = get_dispatcher_shared_arm(si->disp_handle);
    arch_registers_state_t *enabled_area = dispatcher_get_enabled_save_area(si->disp_handle);
    arch_registers_state_t *disabled_area = dispatcher_get_disabled_save_area(si->disp_handle);

    disp_gen->core_id = 0; // we're single-core right now
    disp->udisp = (lvaddr_t)disp_addr_in_child; // Virtual address of the dispatcher frame in childs VSpace
    disp->disabled = 1; // Start in disabled mode
    disp->fpu_trap = 1; // Trap on fpu instructions
    strncpy(disp->name, si->binary_name, DISP_NAME_LEN); // A name (for debugging)

    disabled_area->named.pc = si->entry_point; // Set program counter (where it should start to execute)

    // Initialize offset registers
    disp_arm->got_base = si->got_ubase; // Address of .got in childs VSpace.
    enabled_area->regs[REG_OFFSET(PIC_REGISTER)]  = si->got_ubase; // same as above
    disabled_area->regs[REG_OFFSET(PIC_REGISTER)] = si->got_ubase; // same as above
    enabled_area->named.cpsr = CPSR_F_MASK | ARM_MODE_USR;
    disabled_area->named.cpsr = CPSR_F_MASK | ARM_MODE_USR;
    disp_gen->eh_frame = 0;
    disp_gen->eh_frame_size = 0;
    disp_gen->eh_frame_hdr = 0;
    disp_gen->eh_frame_hdr_size = 0;
    return SYS_ERR_OK;
}

errval_t setup_args(struct spawninfo *si, char *const argv[], int argc) {
    size_t retsize;
    CHECK("allocating frame for args page",
          frame_alloc(&si->argspg, BASE_PAGE_SIZE, &retsize));

    struct capref my_argsframe;
    CHECK("allocating slot for args frame cap", slot_alloc(&my_argsframe));
    CHECK("copying args frame cap", cap_copy(my_argsframe, si->argspg));

    void *args_addr_in_me;
    CHECK("mapping args frame into my vspace",
          paging_map_frame(get_current_paging_state(), &args_addr_in_me,
                           retsize, my_argsframe, NULL, NULL));

    void *args_addr_in_child;
    CHECK("mapping args frame into child's vspace",
          paging_map_frame(&si->pg_state, &args_addr_in_child,
                           retsize, si->argspg, NULL, NULL));

    struct spawn_domain_params *params = (struct spawn_domain_params*)args_addr_in_me;
    memset(params, 0, sizeof(params));

    params->argc = argc;
    CHECK_COND("max number of cmdline args exceeded",
               params->argc <= MAX_CMDLINE_ARGS, SPAWN_ERR_LOAD);
    // CHECK_COND("max number of env vars exceeded",
    //            count_args(envp) <= MAX_ENVIRON_VARS, SPAWN_ERR_LOAD);

    size_t args_offset = ROUND_UP(sizeof(struct spawn_domain_params), 4);
    for (size_t i = 0; argv[i] != NULL; ++i) {
        params->argv[i] = args_addr_in_child + args_offset;
        strcpy((char*) args_addr_in_me + args_offset, argv[i]);
        args_offset += strlen(argv[i]) + 1; // +1 for the terminating NULL.
    }
    ((char*) args_addr_in_me)[args_offset] = '\0';
    // *((char*)(args_addr_in_me + args_offset)) = NULL;
    // ++args_offset;
    // args_offset = ROUND_UP(args_offset, 4);

    // for (size_t i = 0; envp[i] != NULL; ++i) {
    //     params->envp[i] = args_addr_in_child + args_offset;
    //     strcpy((char*) args_addr_in_me + args_offset, envp[i]);
    //     args_offset += strlen(envp[i]) + 1; // +1 for the terminating NULL.
    // }
    // *(char*)(args_addr_in_me + args_offset) = NULL;

    return SYS_ERR_OK;
}

int spawn_tokenize_cmdargs(char *s, char *argv[], size_t argv_len)
{
    bool inquote = false;
    int argc = 0;
    assert(argv_len > 1);
    assert(s != NULL);

    // consume leading whitespace, and mark first argument
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '\0') {
        argv[argc++] = s;
    }

    while (argc + 1 < argv_len && *s != '\0') {
        if (*s == '"') {
            inquote = !inquote;
            // consume quote mark, by moving remainder of string over it
            memmove(s, s + 1, strlen(s));
        } else if ((*s == ' ' || *s == '\t') && !inquote) { // First whitespace, arg finished
            *s++ = '\0';
            while (*s == ' ' || *s == '\t') s++; // Consume trailing whitespace
            if (*s != '\0') { // New arg started
                argv[argc++] = s;
            }
        } else {
            s++;
        }
    }

    argv[argc] = NULL;
    return argc;
}


// TODO(M2): Implement this function such that it starts a new process
// TODO(M4): Build and pass a messaging channel to your child process
errval_t spawn_load_by_name(void * binary_name, struct spawninfo * si) {

    DPRINT("loading and starting: %s", binary_name);

    // Init spawninfo
    memset(si, 0, sizeof(*si));
    si->binary_name = binary_name;

    // - Get the binary from multiboot image

    struct mem_region *module = multiboot_find_module(bi, binary_name);
    if (!module) {
        DPRINT("Module %s not found", binary_name);
        return SPAWN_ERR_FIND_MODULE;
    }

    struct capref child_frame = {
        .cnode = cnode_module,
        .slot = module->mrmod_slot,
    };

    // - Map multiboot module in your address space

    struct frame_identity child_frame_id;
    CHECK("identifying frame",
          frame_identify(child_frame, &child_frame_id));

    lvaddr_t mapped_elf;
    CHECK("mapping frame",
          paging_map_frame(get_current_paging_state(), (void**)&mapped_elf,
                           child_frame_id.bytes, child_frame, NULL, NULL));
    // DPRINT("ELF header: %0x %c %c %c", ((char*)mapped_elf)[0], ((char*)mapped_elf)[1], ((char*)mapped_elf)[2], ((char*)mapped_elf)[3]);

    // this gets checked twice (second time in elf_load)... so what :D
    struct Elf32_Ehdr *elf_header = (void*)mapped_elf;
    if (!IS_ELF(*elf_header)) {
        DPRINT("Module %s is not an ELF executable", binary_name);
        return ELF_ERR_HEADER;
    }

    // - Setup childs cspace

    setup_cspace(si);
    //CHECK("Setup CSPACE", setup_cspace(si));

    // - Setup childs vspace
    setup_vspace(si);

    // - Load the ELF binary
    CHECK("loading ELF", load_elf_into_memory(si, mapped_elf, child_frame_id.bytes));

    // - Setup dispatcher
    CHECK("setting up dispatcher", setup_dispatcher(si));

    // - Setup environment
    // get arguments from menu.lst
    const char *args_string = multiboot_module_opts(module);
    char *args_string_rw = malloc(strlen(args_string)+1);
    strcpy(args_string_rw, args_string);
    char *argv[MAX_CMDLINE_ARGS + 1];
    int argc = spawn_tokenize_cmdargs(args_string_rw, argv, STATIC_ARRAY_LENGTH(argv));
    CHECK("args", setup_args(si, argv, argc));

    // - Make dispatcher runnable
    CHECK("invoking dispatcher",
          invoke_dispatcher(si->dispatcher, cap_dispatcher,
          si->l1_cap, si->pg_state.l1_pagetable,
          si->dispframe, true));

    return SYS_ERR_OK;
}
