#ifndef ELF_H
#define ELF_H

#include "../cpu/vmm.h"

/* Cap on argv entries elf_exec() will pass through — both kernel.c (sizing
 * its local tokenized argv[] before calling elf_exec) and elf.c (sizing the
 * process-side header it builds) use this same constant, so it has to live
 * somewhere both can see, unlike elf.c's other ELF_* constants. */
#define ELF_MAX_ARGS 8

/* Loads a static, non-PIE x86_64 ELF64 executable from `path` (any mounted
 * filesystem, fs/vfs.h) into a fresh private address space (cpu/vmm.h) and
 * submits it as a ring3 task (cpu/sched.h: sched_submit_user_as). Only
 * PT_LOAD segments are processed — no dynamic linking, no relocation.
 * All PT_LOAD virtual addresses must fall inside
 * [VMM_USER_BASE, VMM_USER_BASE+VMM_USER_SIZE) (userland/user.ld pins
 * companion programs there).
 *
 * `argv`/`argc` (argv[0] is conventionally `path` itself, same as a Unix
 * exec — the caller decides, elf_exec doesn't enforce it) are copied into a
 * dedicated page inside the new process's own address space, mapped just
 * below its stack, and a pointer to that page's process-side header is
 * passed as `_start`'s `arg` — see kernel/elf.c's local proc_args_t and its
 * mirror, userland/usyscall.h's proc_args_t (kernel and userland headers
 * are never shared, see usyscall.h, but this struct's layout has to match
 * byte-for-byte). Only the first ELF_MAX_ARGS entries of `argv` survive;
 * the rest are silently dropped, same "cap and continue" policy as the
 * syscall layer (cpu/syscall.c).
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
int elf_exec(const char *path, const char *name, int argc, char *const argv[],
            vmm_as_t **out_as);

#endif
