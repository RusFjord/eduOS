/*
 * Copyright (c) 2010, Stefan Lankes, RWTH Aachen University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @author Stefan Lankes
 * @file arch/x86/include/asm/tasks_types.h
 * @brief Task related structure definitions
 *
 * This file contains the task_t structure definition 
 * and task state define constants
 */

#ifndef __ASM_TASKS_TYPES_H__
#define __ASM_TASKS_TYPES_H__

#include <eduos/stddef.h>
#include <asm/processor.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	long	cwd;
	long	swd;
	long	twd;
	long	fip;
	long	fcs;
	long	foo;
	long	fos;
	long	st_space[20];
	long	status;
} i387_fsave_t;

typedef struct i387_fxsave_struct {
	unsigned short	cwd;
	unsigned short	swd;
	unsigned short	twd;
	unsigned short	fop;
	long	fip;
	long	fcs;
	long	foo;
	long	fos;
	long	mxcsr;
	long	mxcsr_mask;
	long	st_space[32];
	long	xmm_space[64];
	long	padding[12];
	union {
		long	padding1[12];
		long	sw_reserved[12];
	};
} i387_fxsave_t __attribute__ ((aligned (16)));

union fpu_state {
	i387_fsave_t	fsave;
	i387_fxsave_t	fxsave;
};

typedef void (*handle_fpu_state)(union fpu_state* state);

extern handle_fpu_state save_fpu_state;
extern handle_fpu_state restore_fpu_state;
extern handle_fpu_state fpu_init;

#ifdef __cplusplus
}
#endif

#endif
