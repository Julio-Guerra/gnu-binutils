#ifndef TIC4X_TDEP_H_
# define TIC4X_TDEP_H_

#define C4X_NUM_REGS 48

#define C4X_REGISTER_NAMES {\
 	"r0", "r1", "r2", "r3",	"r4", "r5", "r6", "r7",\
 	"ar0", "ar1", "ar2", "ar3", "ar4", "ar5", "ar6", "ar7",\
	"dp",\
	"ir0", "ir1",\
	"bk",\
	"sp",\
	"st",\
	"die", "iie", "iif",\
	"rs", "re", "rc",\
	"r8", "r9", "r10", "r11",\
	"ivtp", "tvtp",\
	"pc",\
 	"er0", "er1", "er2", "er3", "er4", "er5", "er6", "er7",\
	"er8", "er9", "er10", "er11", "clk"\
	}

enum tic4x_regnum
{
	/* Extended-precision registers */
	TIC4X_R0_REGNUM   = 0,
	TIC4X_R1_REGNUM   = 1,
	TIC4X_R2_REGNUM   = 2,
	TIC4X_R3_REGNUM   = 3,
	TIC4X_R4_REGNUM   = 4,
	TIC4X_R5_REGNUM   = 5,
	TIC4X_R6_REGNUM   = 6,
	TIC4X_R7_REGNUM   = 7,
	/* Auxiliary registers */
	TIC4X_AR0_REGNUM  = 8,
	TIC4X_AR1_REGNUM  = 9,
	TIC4X_AR2_REGNUM  = 10,
	TIC4X_AR3_REGNUM  = 11,
	TIC4X_AR4_REGNUM  = 12,
	TIC4X_AR5_REGNUM  = 13,
	TIC4X_AR6_REGNUM  = 14,
	TIC4X_AR7_REGNUM  = 15,
	/* Data page register */
	TIC4X_DP_REGNUM   = 16,
	/* Index registers */
	TIC4X_IR0_REGNUM  = 17,
	TIC4X_IR1_REGNUM  = 18,
	/* Block size register */
	TIC4X_BK_REGNUM   = 19,
	/* Stack pointer */
	TIC4X_SP_REGNUM   = 20,
	/* Status register */
	TIC4X_ST_REGNUM   = 21,
	/* Misc interrupt registers */
	TIC4X_DIE_REGNUM  = 22,
	TIC3X_IE_REGNUM   = 22,
	TIC4X_IIE_REGNUM  = 23,
	TIC3X_IF_REGNUM   = 23,
	TIC4X_IIF_REGNUM  = 24,
	TIC3X_IOF_REGNUM  = 24,
	/* Repeat block registers */
	TIC4X_RS_REGNUM   = 25,
	TIC4X_RE_REGNUM   = 26,
	TIC4X_RC_REGNUM   = 27,
	/* Additional extended-precision registers */
	TIC4X_R8_REGNUM   = 28,
	TIC4X_R9_REGNUM   = 29,
	TIC4X_R10_REGNUM  = 30,
	TIC4X_R11_REGNUM  = 31,
	/* Vector table pointers */
	TIC4X_TVTP_REGNUM = 32,
	TIC4X_IVTP_REGNUM = 33,
	/* Program counter */
	TIC4X_PC_REGNUM   = 34,
	/* MSBs of extended-precision registers */
	TIC4X_ER0_REGNUM  = 35,
	TIC4X_ER1_REGNUM  = 36,
	TIC4X_ER2_REGNUM  = 37,
	TIC4X_ER3_REGNUM  = 38,
	TIC4X_ER4_REGNUM  = 39,
	TIC4X_ER5_REGNUM  = 40,
	TIC4X_ER6_REGNUM  = 41,
	TIC4X_ER7_REGNUM  = 42,
	TIC4X_ER8_REGNUM  = 43,
	TIC4X_ER9_REGNUM  = 44,
	TIC4X_ER10_REGNUM = 45,
	TIC4X_ER11_REGNUM = 46,
	TIC4X_CLK_REGNUM  = 47,

  TIC4X_NUM_REGS    = 48
};

void
_initialize_tic4x_tdep (void);

#endif /* !TIC4X_TDEP_H_ */
