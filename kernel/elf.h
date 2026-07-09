#ifndef ELF_H
#define ELF_H

#include "../cpu/vmm.h"

/* Loads a static, non-PIE x86_64 ELF64 executable from `path` (any mounted
 * filesystem, fs/vfs.h) into a fresh private address space (cpu/vmm.h) and
 * submits it as a ring3 task (cpu/sched.h: sched_submit_user_as). Only
 * PT_LOAD segments are processed — no dynamic linking, no relocation, no
 * argv/envp. All PT_LOAD virtual addresses must fall inside
 * [VMM_USER_BASE, VMM_USER_BASE+VMM_USER_SIZE) (userland/user.ld pins
 * companion programs there).
 *
 * On success returns the sched_get_task() slot for the new task and sets
 * *out_as to the address space (caller must wait for TASK_DONE, then call
 * vmm_destroy_address_space(*out_as) — elf_exec() doesn't do that itself,
 * since the caller decides how/when to wait, same as the `wm`/`ring3test`
 * shell commands in kernel/kernel.c). Returns -1 on any failure (bad path,
 * malformed ELF, segment outside the process window, OOM) and leaves
 * *out_as untouched/NULL — nothing partially submitted needs cleanup on
 * the failure path except whatever vmm_create_address_space had already
 * allocated, which elf_exec frees itself before returning. */
int elf_exec(const char *path, const char *name, vmm_as_t **out_as);

#endif
