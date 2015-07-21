/* Target-dependent code for the TMS320 C3X/C4X.

   Copyright (C) 1986-2015 Free Software Foundation, Inc.

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
#include "arch-utils.h"
#include "dis-asm.h"
#include "floatformat.h"
#include "frame.h"
#include "frame-base.h"
#include "frame-unwind.h"
#include "gdbcore.h"
#include "gdbtypes.h"
#include "osabi.h"
#include "regcache.h"
#include "regset.h"
#include "trad-frame.h"
#include "value.h"

#include "tic4x-tdep.h"

static CORE_ADDR
tic4x_skip_prologue (struct gdbarch *gdbarch, CORE_ADDR pc)
{
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  unsigned int    op;

  op = read_memory_unsigned_integer (pc, 4, byte_order);
  if (op == 0x0f2b0000U)  /* push fp (ar3) */
  {
    pc += 4;
    op = read_memory_unsigned_integer (pc, 4, byte_order);
    if (op == 0x080b0014U)  /* ldi sp,fp */
    {
      pc += 4;
      op = read_memory_unsigned_integer (pc, 4, byte_order);
      if ((op & 0xffff0000U) == 0x02740000U) /* addi x,sp */
        pc += 4;
      /* We should test for the case where the size of local
         variables is greater than 32767... */
    }
  }

  return pc;
}

/* Return the name of register REGNUM.  */

static const char *
tic4x_register_name (struct gdbarch *gdbarch, int regnum)
{
  static char *register_names[TIC4X_NUM_REGS] =
  {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "ar0", "ar1", "ar2", "ar3", "ar4", "ar5", "ar6", "ar7",
    "dp",
    "ir0", "ir1",
    "bk",
    "sp",
    "st",
    "die", "iie", "iif",
    "rs", "re", "rc",
    "r8", "r9", "r10", "r11",
    "ivtp", "tvtp",
    "pc",
    "er0", "er1", "er2", "er3", "er4", "er5", "er6", "er7",
    "er8", "er9", "er10", "er11", "clk"
  };

  if (regnum >= 0 && regnum < TIC4X_NUM_REGS)
    return register_names[regnum];

  return NULL;
}

/* Use the program counter to determine the contents and size of a
   breakpoint instruction.  Return a pointer to a string of bytes that
   encode a breakpoint instruction, store the length of the string in
   *LEN and optionally adjust *PC to point to the correct memory
   location for inserting the breakpoint.  */

static const gdb_byte *
tic4x_breakpoint_from_pc (struct gdbarch *gdbarch, CORE_ADDR *pc, int *len)
{
  static gdb_byte break_insn[] = {0x3f, 0x00, 0x00, 0x74} /* TRAP 0x1f */;

  *len = sizeof (break_insn);
  return break_insn;
}

/* Return the return-value convention that will be used by FUNCTION
   to return a value of type VALTYPE.  FUNCTION may be NULL in which
   case the return convention is computed based only on VALTYPE.

   If READBUF is not NULL, extract the return value and save it in this buffer.

   If WRITEBUF is not NULL, it contains a return value which will be
   stored into the appropriate register.  This can be used when we want
   to force the value returned by a function (see the "return" command
   for instance). */

static enum return_value_convention
tic4x_return_value (struct gdbarch  *gdbarch,
                    struct value    *function,
                    struct type     *type,
                    struct regcache *regcache,
                    gdb_byte        *readbuf,
                    const gdb_byte  *writebuf)
{
  int len = TYPE_LENGTH (type);
  gdb_byte buf[8];

  if (TYPE_CODE (type) == TYPE_CODE_STRUCT
      || TYPE_CODE (type) == TYPE_CODE_UNION
      || TYPE_CODE (type) == TYPE_CODE_ARRAY)
  {
    if (readbuf)
    {
      ULONGEST addr;

      regcache_raw_read_unsigned (regcache, TIC4X_R0_REGNUM, &addr);
      read_memory (addr, readbuf, len);
    }

    // todo: abi defined ou simple convetion. cf defs.h
    return RETURN_VALUE_ABI_RETURNS_ADDRESS;
  }

  if (readbuf)
  {
    /* Read the contents of R0 and (if necessary) R1.  */
    regcache_cooked_read (regcache, TIC4X_R0_REGNUM, buf);
    if (len > 4)
      regcache_cooked_read (regcache, TIC4X_R1_REGNUM, buf + 4);
    memcpy (readbuf, buf, len);
  }

  if (writebuf)
  {
    /* Read the contents to R0 and (if necessary) R1.  */
    memcpy (buf, writebuf, len);
    regcache_cooked_write (regcache, TIC4X_R0_REGNUM, buf);
    if (len > 4)
      regcache_cooked_write (regcache, TIC4X_R1_REGNUM, buf + 4);
  }

  return RETURN_VALUE_REGISTER_CONVENTION;
}

/* Return number of arguments for FRAME.  */

static int
tic4x_frame_num_args (struct frame_info *frame)
{
  return -1;
}

/* static CORE_ADDR */
/* tic4x_frame_args_address (struct frame_info *this_frame, void **this_cache) */
/* { */
/* 	// addresse de base de la frame courante apparemment */
/*   return this_frame->frame; */
/* } */

/* Initialize the current architecture based on INFO.  If possible, re-use an
   architecture from ARCHES, which is a list of architectures already created
   during this debugging session. */

static struct gdbarch *
tic4x_gdbarch_init (struct gdbarch_info info, struct gdbarch_list *arches)
{
  struct gdbarch *gdbarch;

  arches = gdbarch_list_lookup_by_info (arches, &info);
  if (arches != NULL)
    return arches->gdbarch;

  gdbarch = gdbarch_alloc (&info, NULL);

  set_gdbarch_short_bit (gdbarch, 32);
  set_gdbarch_int_bit (gdbarch, 32);
  set_gdbarch_long_bit (gdbarch, 32);
  set_gdbarch_float_bit (gdbarch, 32);
  set_gdbarch_double_bit (gdbarch, 32);

  /* Register info */
  set_gdbarch_num_regs (gdbarch, TIC4X_NUM_REGS);
  set_gdbarch_register_name (gdbarch, tic4x_register_name);
  set_gdbarch_sp_regnum (gdbarch, TIC4X_SP_REGNUM);
  set_gdbarch_pc_regnum (gdbarch, TIC4X_PC_REGNUM);
  set_gdbarch_ps_regnum (gdbarch, TIC4X_ST_REGNUM);

  /* Frame and stack info */
  set_gdbarch_skip_prologue (gdbarch, tic4x_skip_prologue);
  set_gdbarch_frame_num_args (gdbarch, tic4x_frame_num_args);
  set_gdbarch_frame_args_skip (gdbarch, 0);

  /* Stack grows downward.  */
  set_gdbarch_inner_than (gdbarch, core_addr_lessthan);

  /* Return value info */
  set_gdbarch_return_value (gdbarch, tic4x_return_value);

  /* Breakpoint info */
  set_gdbarch_breakpoint_from_pc (gdbarch, tic4x_breakpoint_from_pc);

  set_gdbarch_print_insn (gdbarch, print_insn_tic4x);

	//  frame_base_set_default (gdbarch, &tic4x_frame_base);

  return gdbarch;
}

void
_initialize_tic4x_tdep (void)
{
  gdbarch_register (bfd_arch_tic4x, tic4x_gdbarch_init, NULL);
}
