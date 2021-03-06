/* Native-dependent code for GNU/Linux AArch64.

   Copyright (C) 2011-2015 Free Software Foundation, Inc.
   Contributed by ARM Ltd.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"

#include "inferior.h"
#include "gdbcore.h"
#include "regcache.h"
#include "linux-nat.h"
#include "target-descriptions.h"
#include "auxv.h"
#include "gdbcmd.h"
#include "aarch64-tdep.h"
#include "aarch64-linux-tdep.h"
#include "aarch32-linux-nat.h"
#include "nat/aarch64-linux-hw-point.h"

#include "elf/external.h"
#include "elf/common.h"

#include "nat/gdb_ptrace.h"
#include <sys/utsname.h>
#include <asm/ptrace.h>

#include "gregset.h"

/* Defines ps_err_e, struct ps_prochandle.  */
#include "gdb_proc_service.h"

#ifndef TRAP_HWBKPT
#define TRAP_HWBKPT 0x0004
#endif

/* Per-process data.  We don't bind this to a per-inferior registry
   because of targets like x86 GNU/Linux that need to keep track of
   processes that aren't bound to any inferior (e.g., fork children,
   checkpoints).  */

struct aarch64_process_info
{
  /* Linked list.  */
  struct aarch64_process_info *next;

  /* The process identifier.  */
  pid_t pid;

  /* Copy of aarch64 hardware debug registers.  */
  struct aarch64_debug_reg_state state;
};

static struct aarch64_process_info *aarch64_process_list = NULL;

/* Find process data for process PID.  */

static struct aarch64_process_info *
aarch64_find_process_pid (pid_t pid)
{
  struct aarch64_process_info *proc;

  for (proc = aarch64_process_list; proc; proc = proc->next)
    if (proc->pid == pid)
      return proc;

  return NULL;
}

/* Add process data for process PID.  Returns newly allocated info
   object.  */

static struct aarch64_process_info *
aarch64_add_process (pid_t pid)
{
  struct aarch64_process_info *proc;

  proc = xcalloc (1, sizeof (*proc));
  proc->pid = pid;

  proc->next = aarch64_process_list;
  aarch64_process_list = proc;

  return proc;
}

/* Get data specific info for process PID, creating it if necessary.
   Never returns NULL.  */

static struct aarch64_process_info *
aarch64_process_info_get (pid_t pid)
{
  struct aarch64_process_info *proc;

  proc = aarch64_find_process_pid (pid);
  if (proc == NULL)
    proc = aarch64_add_process (pid);

  return proc;
}

/* Called whenever GDB is no longer debugging process PID.  It deletes
   data structures that keep track of debug register state.  */

static void
aarch64_forget_process (pid_t pid)
{
  struct aarch64_process_info *proc, **proc_link;

  proc = aarch64_process_list;
  proc_link = &aarch64_process_list;

  while (proc != NULL)
    {
      if (proc->pid == pid)
	{
	  *proc_link = proc->next;

	  xfree (proc);
	  return;
	}

      proc_link = &proc->next;
      proc = *proc_link;
    }
}

/* Get debug registers state for process PID.  */

static struct aarch64_debug_reg_state *
aarch64_get_debug_reg_state (pid_t pid)
{
  return &aarch64_process_info_get (pid)->state;
}

struct aarch64_dr_update_callback_param
{
  int is_watchpoint;
  unsigned int idx;
};

/* Callback for iterate_over_lwps.  Records the
   information about the change of one hardware breakpoint/watchpoint
   setting for the thread LWP.
   The information is passed in via PTR.
   N.B.  The actual updating of hardware debug registers is not
   carried out until the moment the thread is resumed.  */

static int
debug_reg_change_callback (struct lwp_info *lwp, void *ptr)
{
  struct aarch64_dr_update_callback_param *param_p
    = (struct aarch64_dr_update_callback_param *) ptr;
  int pid = ptid_get_lwp (lwp->ptid);
  int idx = param_p->idx;
  int is_watchpoint = param_p->is_watchpoint;
  struct arch_lwp_info *info = lwp->arch_private;
  dr_changed_t *dr_changed_ptr;
  dr_changed_t dr_changed;

  if (info == NULL)
    info = lwp->arch_private = XCNEW (struct arch_lwp_info);

  if (show_debug_regs)
    {
      fprintf_unfiltered (gdb_stdlog,
			  "debug_reg_change_callback: \n\tOn entry:\n");
      fprintf_unfiltered (gdb_stdlog,
			  "\tpid%d, dr_changed_bp=0x%s, "
			  "dr_changed_wp=0x%s\n",
			  pid, phex (info->dr_changed_bp, 8),
			  phex (info->dr_changed_wp, 8));
    }

  dr_changed_ptr = is_watchpoint ? &info->dr_changed_wp
    : &info->dr_changed_bp;
  dr_changed = *dr_changed_ptr;

  gdb_assert (idx >= 0
	      && (idx <= (is_watchpoint ? aarch64_num_wp_regs
			  : aarch64_num_bp_regs)));

  /* The actual update is done later just before resuming the lwp,
     we just mark that one register pair needs updating.  */
  DR_MARK_N_CHANGED (dr_changed, idx);
  *dr_changed_ptr = dr_changed;

  /* If the lwp isn't stopped, force it to momentarily pause, so
     we can update its debug registers.  */
  if (!lwp->stopped)
    linux_stop_lwp (lwp);

  if (show_debug_regs)
    {
      fprintf_unfiltered (gdb_stdlog,
			  "\tOn exit:\n\tpid%d, dr_changed_bp=0x%s, "
			  "dr_changed_wp=0x%s\n",
			  pid, phex (info->dr_changed_bp, 8),
			  phex (info->dr_changed_wp, 8));
    }

  /* Continue the iteration.  */
  return 0;
}

/* Notify each thread that their IDXth breakpoint/watchpoint register
   pair needs to be updated.  The message will be recorded in each
   thread's arch-specific data area, the actual updating will be done
   when the thread is resumed.  */

void
aarch64_notify_debug_reg_change (const struct aarch64_debug_reg_state *state,
				 int is_watchpoint, unsigned int idx)
{
  struct aarch64_dr_update_callback_param param;
  ptid_t pid_ptid = pid_to_ptid (ptid_get_pid (inferior_ptid));

  param.is_watchpoint = is_watchpoint;
  param.idx = idx;

  iterate_over_lwps (pid_ptid, debug_reg_change_callback, (void *) &param);
}

/* Fill GDB's register array with the general-purpose register values
   from the current thread.  */

static void
fetch_gregs_from_thread (struct regcache *regcache)
{
  int ret, tid;
  struct gdbarch *gdbarch = get_regcache_arch (regcache);
  elf_gregset_t regs;
  struct iovec iovec;

  /* Make sure REGS can hold all registers contents on both aarch64
     and arm.  */
  gdb_static_assert (sizeof (regs) >= 18 * 4);

  tid = ptid_get_lwp (inferior_ptid);

  iovec.iov_base = &regs;
  if (gdbarch_bfd_arch_info (gdbarch)->bits_per_word == 32)
    iovec.iov_len = 18 * 4;
  else
    iovec.iov_len = sizeof (regs);

  ret = ptrace (PTRACE_GETREGSET, tid, NT_PRSTATUS, &iovec);
  if (ret < 0)
    perror_with_name (_("Unable to fetch general registers."));

  if (gdbarch_bfd_arch_info (gdbarch)->bits_per_word == 32)
    aarch32_gp_regcache_supply (regcache, (uint32_t *) regs, 1);
  else
    {
      int regno;

      for (regno = AARCH64_X0_REGNUM; regno <= AARCH64_CPSR_REGNUM; regno++)
	regcache_raw_supply (regcache, regno, &regs[regno - AARCH64_X0_REGNUM]);
    }
}

/* Store to the current thread the valid general-purpose register
   values in the GDB's register array.  */

static void
store_gregs_to_thread (const struct regcache *regcache)
{
  int ret, tid;
  elf_gregset_t regs;
  struct iovec iovec;
  struct gdbarch *gdbarch = get_regcache_arch (regcache);

  /* Make sure REGS can hold all registers contents on both aarch64
     and arm.  */
  gdb_static_assert (sizeof (regs) >= 18 * 4);
  tid = ptid_get_lwp (inferior_ptid);

  iovec.iov_base = &regs;
  if (gdbarch_bfd_arch_info (gdbarch)->bits_per_word == 32)
    iovec.iov_len = 18 * 4;
  else
    iovec.iov_len = sizeof (regs);

  ret = ptrace (PTRACE_GETREGSET, tid, NT_PRSTATUS, &iovec);
  if (ret < 0)
    perror_with_name (_("Unable to fetch general registers."));

  if (gdbarch_bfd_arch_info (gdbarch)->bits_per_word == 32)
    aarch32_gp_regcache_collect (regcache, (uint32_t *) regs, 1);
  else
    {
      int regno;

      for (regno = AARCH64_X0_REGNUM; regno <= AARCH64_CPSR_REGNUM; regno++)
	if (REG_VALID == regcache_register_status (regcache, regno))
	  regcache_raw_collect (regcache, regno,
				&regs[regno - AARCH64_X0_REGNUM]);
    }

  ret = ptrace (PTRACE_SETREGSET, tid, NT_PRSTATUS, &iovec);
  if (ret < 0)
    perror_with_name (_("Unable to store general registers."));
}

/* Fill GDB's register array with the fp/simd register values
   from the current thread.  */

static void
fetch_fpregs_from_thread (struct regcache *regcache)
{
  int ret, tid;
  elf_fpregset_t regs;
  struct iovec iovec;
  struct gdbarch *gdbarch = get_regcache_arch (regcache);

  /* Make sure REGS can hold all VFP registers contents on both aarch64
     and arm.  */
  gdb_static_assert (sizeof regs >= VFP_REGS_SIZE);

  tid = ptid_get_lwp (inferior_ptid);

  iovec.iov_base = &regs;

  if (gdbarch_bfd_arch_info (gdbarch)->bits_per_word == 32)
    {
      iovec.iov_len = VFP_REGS_SIZE;

      ret = ptrace (PTRACE_GETREGSET, tid, NT_ARM_VFP, &iovec);
      if (ret < 0)
	perror_with_name (_("Unable to fetch VFP registers."));

      aarch32_vfp_regcache_supply (regcache, (gdb_byte *) &regs, 32);
    }
  else
    {
      int regno;

      iovec.iov_len = sizeof (regs);

      ret = ptrace (PTRACE_GETREGSET, tid, NT_FPREGSET, &iovec);
      if (ret < 0)
	perror_with_name (_("Unable to fetch vFP/SIMD registers."));

      for (regno = AARCH64_V0_REGNUM; regno <= AARCH64_V31_REGNUM; regno++)
	regcache_raw_supply (regcache, regno,
			     &regs.vregs[regno - AARCH64_V0_REGNUM]);

      regcache_raw_supply (regcache, AARCH64_FPSR_REGNUM, &regs.fpsr);
      regcache_raw_supply (regcache, AARCH64_FPCR_REGNUM, &regs.fpcr);
    }
}

/* Store to the current thread the valid fp/simd register
   values in the GDB's register array.  */

static void
store_fpregs_to_thread (const struct regcache *regcache)
{
  int ret, tid;
  elf_fpregset_t regs;
  struct iovec iovec;
  struct gdbarch *gdbarch = get_regcache_arch (regcache);

  /* Make sure REGS can hold all VFP registers contents on both aarch64
     and arm.  */
  gdb_static_assert (sizeof regs >= VFP_REGS_SIZE);
  tid = ptid_get_lwp (inferior_ptid);

  iovec.iov_base = &regs;

  if (gdbarch_bfd_arch_info (gdbarch)->bits_per_word == 32)
    {
      iovec.iov_len = VFP_REGS_SIZE;

      ret = ptrace (PTRACE_GETREGSET, tid, NT_ARM_VFP, &iovec);
      if (ret < 0)
	perror_with_name (_("Unable to fetch VFP registers."));

      aarch32_vfp_regcache_collect (regcache, (gdb_byte *) &regs, 32);
    }
  else
    {
      int regno;

      iovec.iov_len = sizeof (regs);

      ret = ptrace (PTRACE_GETREGSET, tid, NT_FPREGSET, &iovec);
      if (ret < 0)
	perror_with_name (_("Unable to fetch FP/SIMD registers."));

      for (regno = AARCH64_V0_REGNUM; regno <= AARCH64_V31_REGNUM; regno++)
	if (REG_VALID == regcache_register_status (regcache, regno))
	  regcache_raw_collect (regcache, regno,
				(char *) &regs.vregs[regno - AARCH64_V0_REGNUM]);

      if (REG_VALID == regcache_register_status (regcache, AARCH64_FPSR_REGNUM))
	regcache_raw_collect (regcache, AARCH64_FPSR_REGNUM,
			      (char *) &regs.fpsr);
      if (REG_VALID == regcache_register_status (regcache, AARCH64_FPCR_REGNUM))
	regcache_raw_collect (regcache, AARCH64_FPCR_REGNUM,
			      (char *) &regs.fpcr);
    }

  if (gdbarch_bfd_arch_info (gdbarch)->bits_per_word == 32)
    {
      ret = ptrace (PTRACE_SETREGSET, tid, NT_ARM_VFP, &iovec);
      if (ret < 0)
	perror_with_name (_("Unable to store VFP registers."));
    }
  else
    {
      ret = ptrace (PTRACE_SETREGSET, tid, NT_FPREGSET, &iovec);
      if (ret < 0)
	perror_with_name (_("Unable to store FP/SIMD registers."));
    }
}

/* Implement the "to_fetch_register" target_ops method.  */

static void
aarch64_linux_fetch_inferior_registers (struct target_ops *ops,
					struct regcache *regcache,
					int regno)
{
  if (regno == -1)
    {
      fetch_gregs_from_thread (regcache);
      fetch_fpregs_from_thread (regcache);
    }
  else if (regno < AARCH64_V0_REGNUM)
    fetch_gregs_from_thread (regcache);
  else
    fetch_fpregs_from_thread (regcache);
}

/* Implement the "to_store_register" target_ops method.  */

static void
aarch64_linux_store_inferior_registers (struct target_ops *ops,
					struct regcache *regcache,
					int regno)
{
  if (regno == -1)
    {
      store_gregs_to_thread (regcache);
      store_fpregs_to_thread (regcache);
    }
  else if (regno < AARCH64_V0_REGNUM)
    store_gregs_to_thread (regcache);
  else
    store_fpregs_to_thread (regcache);
}

/* Fill register REGNO (if it is a general-purpose register) in
   *GREGSETPS with the value in GDB's register array.  If REGNO is -1,
   do this for all registers.  */

void
fill_gregset (const struct regcache *regcache,
	      gdb_gregset_t *gregsetp, int regno)
{
  regcache_collect_regset (&aarch64_linux_gregset, regcache,
			   regno, (gdb_byte *) gregsetp,
			   AARCH64_LINUX_SIZEOF_GREGSET);
}

/* Fill GDB's register array with the general-purpose register values
   in *GREGSETP.  */

void
supply_gregset (struct regcache *regcache, const gdb_gregset_t *gregsetp)
{
  regcache_supply_regset (&aarch64_linux_gregset, regcache, -1,
			  (const gdb_byte *) gregsetp,
			  AARCH64_LINUX_SIZEOF_GREGSET);
}

/* Fill register REGNO (if it is a floating-point register) in
   *FPREGSETP with the value in GDB's register array.  If REGNO is -1,
   do this for all registers.  */

void
fill_fpregset (const struct regcache *regcache,
	       gdb_fpregset_t *fpregsetp, int regno)
{
  regcache_collect_regset (&aarch64_linux_fpregset, regcache,
			   regno, (gdb_byte *) fpregsetp,
			   AARCH64_LINUX_SIZEOF_FPREGSET);
}

/* Fill GDB's register array with the floating-point register values
   in *FPREGSETP.  */

void
supply_fpregset (struct regcache *regcache, const gdb_fpregset_t *fpregsetp)
{
  regcache_supply_regset (&aarch64_linux_fpregset, regcache, -1,
			  (const gdb_byte *) fpregsetp,
			  AARCH64_LINUX_SIZEOF_FPREGSET);
}

/* Called when resuming a thread.
   The hardware debug registers are updated when there is any change.  */

static void
aarch64_linux_prepare_to_resume (struct lwp_info *lwp)
{
  struct arch_lwp_info *info = lwp->arch_private;

  /* NULL means this is the main thread still going through the shell,
     or, no watchpoint has been set yet.  In that case, there's
     nothing to do.  */
  if (info == NULL)
    return;

  if (DR_HAS_CHANGED (info->dr_changed_bp)
      || DR_HAS_CHANGED (info->dr_changed_wp))
    {
      int tid = ptid_get_lwp (lwp->ptid);
      struct aarch64_debug_reg_state *state
	= aarch64_get_debug_reg_state (ptid_get_pid (lwp->ptid));

      if (show_debug_regs)
	fprintf_unfiltered (gdb_stdlog, "prepare_to_resume thread %d\n", tid);

      /* Watchpoints.  */
      if (DR_HAS_CHANGED (info->dr_changed_wp))
	{
	  aarch64_linux_set_debug_regs (state, tid, 1);
	  DR_CLEAR_CHANGED (info->dr_changed_wp);
	}

      /* Breakpoints.  */
      if (DR_HAS_CHANGED (info->dr_changed_bp))
	{
	  aarch64_linux_set_debug_regs (state, tid, 0);
	  DR_CLEAR_CHANGED (info->dr_changed_bp);
	}
    }
}

static void
aarch64_linux_new_thread (struct lwp_info *lp)
{
  struct arch_lwp_info *info = XCNEW (struct arch_lwp_info);

  /* Mark that all the hardware breakpoint/watchpoint register pairs
     for this thread need to be initialized.  */
  DR_MARK_ALL_CHANGED (info->dr_changed_bp, aarch64_num_bp_regs);
  DR_MARK_ALL_CHANGED (info->dr_changed_wp, aarch64_num_wp_regs);

  lp->arch_private = info;
}

/* linux_nat_new_fork hook.   */

static void
aarch64_linux_new_fork (struct lwp_info *parent, pid_t child_pid)
{
  pid_t parent_pid;
  struct aarch64_debug_reg_state *parent_state;
  struct aarch64_debug_reg_state *child_state;

  /* NULL means no watchpoint has ever been set in the parent.  In
     that case, there's nothing to do.  */
  if (parent->arch_private == NULL)
    return;

  /* GDB core assumes the child inherits the watchpoints/hw
     breakpoints of the parent, and will remove them all from the
     forked off process.  Copy the debug registers mirrors into the
     new process so that all breakpoints and watchpoints can be
     removed together.  */

  parent_pid = ptid_get_pid (parent->ptid);
  parent_state = aarch64_get_debug_reg_state (parent_pid);
  child_state = aarch64_get_debug_reg_state (child_pid);
  *child_state = *parent_state;
}


/* Called by libthread_db.  Returns a pointer to the thread local
   storage (or its descriptor).  */

ps_err_e
ps_get_thread_area (const struct ps_prochandle *ph,
		    lwpid_t lwpid, int idx, void **base)
{
  struct iovec iovec;
  uint64_t reg;

  iovec.iov_base = &reg;
  iovec.iov_len = sizeof (reg);

  if (ptrace (PTRACE_GETREGSET, lwpid, NT_ARM_TLS, &iovec) != 0)
    return PS_ERR;

  /* IDX is the bias from the thread pointer to the beginning of the
     thread descriptor.  It has to be subtracted due to implementation
     quirks in libthread_db.  */
  *base = (void *) (reg - idx);

  return PS_OK;
}


static void (*super_post_startup_inferior) (struct target_ops *self,
					    ptid_t ptid);

/* Implement the "to_post_startup_inferior" target_ops method.  */

static void
aarch64_linux_child_post_startup_inferior (struct target_ops *self,
					   ptid_t ptid)
{
  aarch64_forget_process (ptid_get_pid (ptid));
  aarch64_linux_get_debug_reg_capacity (ptid_get_pid (ptid));
  super_post_startup_inferior (self, ptid);
}

extern struct target_desc *tdesc_arm_with_vfpv3;
extern struct target_desc *tdesc_arm_with_neon;

/* Implement the "to_read_description" target_ops method.  */

static const struct target_desc *
aarch64_linux_read_description (struct target_ops *ops)
{
  CORE_ADDR at_phent;

  if (target_auxv_search (ops, AT_PHENT, &at_phent) == 1)
    {
      if (at_phent == sizeof (Elf64_External_Phdr))
	return tdesc_aarch64;
      else
	{
	  CORE_ADDR arm_hwcap = 0;

	  if (target_auxv_search (ops, AT_HWCAP, &arm_hwcap) != 1)
	    return ops->beneath->to_read_description (ops->beneath);

#ifndef COMPAT_HWCAP_VFP
#define COMPAT_HWCAP_VFP        (1 << 6)
#endif
#ifndef COMPAT_HWCAP_NEON
#define COMPAT_HWCAP_NEON       (1 << 12)
#endif
#ifndef COMPAT_HWCAP_VFPv3
#define COMPAT_HWCAP_VFPv3      (1 << 13)
#endif

	  if (arm_hwcap & COMPAT_HWCAP_VFP)
	    {
	      char *buf;
	      const struct target_desc *result = NULL;

	      if (arm_hwcap & COMPAT_HWCAP_NEON)
		result = tdesc_arm_with_neon;
	      else if (arm_hwcap & COMPAT_HWCAP_VFPv3)
		result = tdesc_arm_with_vfpv3;

	      return result;
	    }

	  return NULL;
	}
    }

  return tdesc_aarch64;
}

/* Returns the number of hardware watchpoints of type TYPE that we can
   set.  Value is positive if we can set CNT watchpoints, zero if
   setting watchpoints of type TYPE is not supported, and negative if
   CNT is more than the maximum number of watchpoints of type TYPE
   that we can support.  TYPE is one of bp_hardware_watchpoint,
   bp_read_watchpoint, bp_write_watchpoint, or bp_hardware_breakpoint.
   CNT is the number of such watchpoints used so far (including this
   one).  OTHERTYPE is non-zero if other types of watchpoints are
   currently enabled.  */

static int
aarch64_linux_can_use_hw_breakpoint (struct target_ops *self,
				     enum bptype type,
				     int cnt, int othertype)
{
  if (type == bp_hardware_watchpoint || type == bp_read_watchpoint
      || type == bp_access_watchpoint || type == bp_watchpoint)
    {
      if (aarch64_num_wp_regs == 0)
	return 0;
    }
  else if (type == bp_hardware_breakpoint)
    {
      if (aarch64_num_bp_regs == 0)
	return 0;
    }
  else
    gdb_assert_not_reached ("unexpected breakpoint type");

  /* We always return 1 here because we don't have enough information
     about possible overlap of addresses that they want to watch.  As an
     extreme example, consider the case where all the watchpoints watch
     the same address and the same region length: then we can handle a
     virtually unlimited number of watchpoints, due to debug register
     sharing implemented via reference counts.  */
  return 1;
}

/* Insert a hardware-assisted breakpoint at BP_TGT->reqstd_address.
   Return 0 on success, -1 on failure.  */

static int
aarch64_linux_insert_hw_breakpoint (struct target_ops *self,
				    struct gdbarch *gdbarch,
				    struct bp_target_info *bp_tgt)
{
  int ret;
  CORE_ADDR addr = bp_tgt->placed_address = bp_tgt->reqstd_address;
  const int len = 4;
  const enum target_hw_bp_type type = hw_execute;
  struct aarch64_debug_reg_state *state
    = aarch64_get_debug_reg_state (ptid_get_pid (inferior_ptid));

  if (show_debug_regs)
    fprintf_unfiltered
      (gdb_stdlog,
       "insert_hw_breakpoint on entry (addr=0x%08lx, len=%d))\n",
       (unsigned long) addr, len);

  ret = aarch64_handle_breakpoint (type, addr, len, 1 /* is_insert */, state);

  if (show_debug_regs)
    {
      aarch64_show_debug_reg_state (state,
				    "insert_hw_breakpoint", addr, len, type);
    }

  return ret;
}

/* Remove a hardware-assisted breakpoint at BP_TGT->placed_address.
   Return 0 on success, -1 on failure.  */

static int
aarch64_linux_remove_hw_breakpoint (struct target_ops *self,
				    struct gdbarch *gdbarch,
				    struct bp_target_info *bp_tgt)
{
  int ret;
  CORE_ADDR addr = bp_tgt->placed_address;
  const int len = 4;
  const enum target_hw_bp_type type = hw_execute;
  struct aarch64_debug_reg_state *state
    = aarch64_get_debug_reg_state (ptid_get_pid (inferior_ptid));

  if (show_debug_regs)
    fprintf_unfiltered
      (gdb_stdlog, "remove_hw_breakpoint on entry (addr=0x%08lx, len=%d))\n",
       (unsigned long) addr, len);

  ret = aarch64_handle_breakpoint (type, addr, len, 0 /* is_insert */, state);

  if (show_debug_regs)
    {
      aarch64_show_debug_reg_state (state,
				    "remove_hw_watchpoint", addr, len, type);
    }

  return ret;
}

/* Implement the "to_insert_watchpoint" target_ops method.

   Insert a watchpoint to watch a memory region which starts at
   address ADDR and whose length is LEN bytes.  Watch memory accesses
   of the type TYPE.  Return 0 on success, -1 on failure.  */

static int
aarch64_linux_insert_watchpoint (struct target_ops *self,
				 CORE_ADDR addr, int len,
				 enum target_hw_bp_type type,
				 struct expression *cond)
{
  int ret;
  struct aarch64_debug_reg_state *state
    = aarch64_get_debug_reg_state (ptid_get_pid (inferior_ptid));

  if (show_debug_regs)
    fprintf_unfiltered (gdb_stdlog,
			"insert_watchpoint on entry (addr=0x%08lx, len=%d)\n",
			(unsigned long) addr, len);

  gdb_assert (type != hw_execute);

  ret = aarch64_handle_watchpoint (type, addr, len, 1 /* is_insert */, state);

  if (show_debug_regs)
    {
      aarch64_show_debug_reg_state (state,
				    "insert_watchpoint", addr, len, type);
    }

  return ret;
}

/* Implement the "to_remove_watchpoint" target_ops method.
   Remove a watchpoint that watched the memory region which starts at
   address ADDR, whose length is LEN bytes, and for accesses of the
   type TYPE.  Return 0 on success, -1 on failure.  */

static int
aarch64_linux_remove_watchpoint (struct target_ops *self,
				 CORE_ADDR addr, int len,
				 enum target_hw_bp_type type,
				 struct expression *cond)
{
  int ret;
  struct aarch64_debug_reg_state *state
    = aarch64_get_debug_reg_state (ptid_get_pid (inferior_ptid));

  if (show_debug_regs)
    fprintf_unfiltered (gdb_stdlog,
			"remove_watchpoint on entry (addr=0x%08lx, len=%d)\n",
			(unsigned long) addr, len);

  gdb_assert (type != hw_execute);

  ret = aarch64_handle_watchpoint (type, addr, len, 0 /* is_insert */, state);

  if (show_debug_regs)
    {
      aarch64_show_debug_reg_state (state,
				    "remove_watchpoint", addr, len, type);
    }

  return ret;
}

/* Implement the "to_region_ok_for_hw_watchpoint" target_ops method.  */

static int
aarch64_linux_region_ok_for_hw_watchpoint (struct target_ops *self,
					   CORE_ADDR addr, int len)
{
  CORE_ADDR aligned_addr;

  /* Can not set watchpoints for zero or negative lengths.  */
  if (len <= 0)
    return 0;

  /* Must have hardware watchpoint debug register(s).  */
  if (aarch64_num_wp_regs == 0)
    return 0;

  /* We support unaligned watchpoint address and arbitrary length,
     as long as the size of the whole watched area after alignment
     doesn't exceed size of the total area that all watchpoint debug
     registers can watch cooperatively.

     This is a very relaxed rule, but unfortunately there are
     limitations, e.g. false-positive hits, due to limited support of
     hardware debug registers in the kernel.  See comment above
     aarch64_align_watchpoint for more information.  */

  aligned_addr = addr & ~(AARCH64_HWP_MAX_LEN_PER_REG - 1);
  if (aligned_addr + aarch64_num_wp_regs * AARCH64_HWP_MAX_LEN_PER_REG
      < addr + len)
    return 0;

  /* All tests passed so we are likely to be able to set the watchpoint.
     The reason that it is 'likely' rather than 'must' is because
     we don't check the current usage of the watchpoint registers, and
     there may not be enough registers available for this watchpoint.
     Ideally we should check the cached debug register state, however
     the checking is costly.  */
  return 1;
}

/* Implement the "to_stopped_data_address" target_ops method.  */

static int
aarch64_linux_stopped_data_address (struct target_ops *target,
				    CORE_ADDR *addr_p)
{
  siginfo_t siginfo;
  int i, tid;
  struct aarch64_debug_reg_state *state;

  if (!linux_nat_get_siginfo (inferior_ptid, &siginfo))
    return 0;

  /* This must be a hardware breakpoint.  */
  if (siginfo.si_signo != SIGTRAP
      || (siginfo.si_code & 0xffff) != TRAP_HWBKPT)
    return 0;

  /* Check if the address matches any watched address.  */
  state = aarch64_get_debug_reg_state (ptid_get_pid (inferior_ptid));
  for (i = aarch64_num_wp_regs - 1; i >= 0; --i)
    {
      const unsigned int len = aarch64_watchpoint_length (state->dr_ctrl_wp[i]);
      const CORE_ADDR addr_trap = (CORE_ADDR) siginfo.si_addr;
      const CORE_ADDR addr_watch = state->dr_addr_wp[i];

      if (state->dr_ref_count_wp[i]
	  && DR_CONTROL_ENABLED (state->dr_ctrl_wp[i])
	  && addr_trap >= addr_watch
	  && addr_trap < addr_watch + len)
	{
	  *addr_p = addr_trap;
	  return 1;
	}
    }

  return 0;
}

/* Implement the "to_stopped_by_watchpoint" target_ops method.  */

static int
aarch64_linux_stopped_by_watchpoint (struct target_ops *ops)
{
  CORE_ADDR addr;

  return aarch64_linux_stopped_data_address (ops, &addr);
}

/* Implement the "to_watchpoint_addr_within_range" target_ops method.  */

static int
aarch64_linux_watchpoint_addr_within_range (struct target_ops *target,
					    CORE_ADDR addr,
					    CORE_ADDR start, int length)
{
  return start <= addr && start + length - 1 >= addr;
}

/* Define AArch64 maintenance commands.  */

static void
add_show_debug_regs_command (void)
{
  /* A maintenance command to enable printing the internal DRi mirror
     variables.  */
  add_setshow_boolean_cmd ("show-debug-regs", class_maintenance,
			   &show_debug_regs, _("\
Set whether to show variables that mirror the AArch64 debug registers."), _("\
Show whether to show variables that mirror the AArch64 debug registers."), _("\
Use \"on\" to enable, \"off\" to disable.\n\
If enabled, the debug registers values are shown when GDB inserts\n\
or removes a hardware breakpoint or watchpoint, and when the inferior\n\
triggers a breakpoint or watchpoint."),
			   NULL,
			   NULL,
			   &maintenance_set_cmdlist,
			   &maintenance_show_cmdlist);
}

/* -Wmissing-prototypes.  */
void _initialize_aarch64_linux_nat (void);

void
_initialize_aarch64_linux_nat (void)
{
  struct target_ops *t;

  /* Fill in the generic GNU/Linux methods.  */
  t = linux_target ();

  add_show_debug_regs_command ();

  /* Add our register access methods.  */
  t->to_fetch_registers = aarch64_linux_fetch_inferior_registers;
  t->to_store_registers = aarch64_linux_store_inferior_registers;

  t->to_read_description = aarch64_linux_read_description;

  t->to_can_use_hw_breakpoint = aarch64_linux_can_use_hw_breakpoint;
  t->to_insert_hw_breakpoint = aarch64_linux_insert_hw_breakpoint;
  t->to_remove_hw_breakpoint = aarch64_linux_remove_hw_breakpoint;
  t->to_region_ok_for_hw_watchpoint =
    aarch64_linux_region_ok_for_hw_watchpoint;
  t->to_insert_watchpoint = aarch64_linux_insert_watchpoint;
  t->to_remove_watchpoint = aarch64_linux_remove_watchpoint;
  t->to_stopped_by_watchpoint = aarch64_linux_stopped_by_watchpoint;
  t->to_stopped_data_address = aarch64_linux_stopped_data_address;
  t->to_watchpoint_addr_within_range =
    aarch64_linux_watchpoint_addr_within_range;

  /* Override the GNU/Linux inferior startup hook.  */
  super_post_startup_inferior = t->to_post_startup_inferior;
  t->to_post_startup_inferior = aarch64_linux_child_post_startup_inferior;

  /* Register the target.  */
  linux_nat_add_target (t);
  linux_nat_set_new_thread (t, aarch64_linux_new_thread);
  linux_nat_set_new_fork (t, aarch64_linux_new_fork);
  linux_nat_set_forget_process (t, aarch64_forget_process);
  linux_nat_set_prepare_to_resume (t, aarch64_linux_prepare_to_resume);
}
