// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/signal.c
 *
 * Copyright (C) 1995-2009 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/cache.h>
#include <linux/compat.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/personality.h>
#include <linux/freezer.h>
#include <linux/stddef.h>
#include <linux/uaccess.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/tracehook.h>
#include <linux/ratelimit.h>
#include <linux/syscalls.h>

#include <asm/daifflags.h>
#include <asm/debug-monitors.h>
#include <asm/elf.h>
#include <asm/cacheflush.h>
#include <asm/ucontext.h>
#include <asm/unistd.h>
#include <asm/fpsimd.h>
#include <asm/ptrace.h>
#include <asm/signal32.h>
#include <asm/traps.h>
#include <asm/vdso.h>

struct ilp32_ucontext {
	u32		uc_flags;
	u32		uc_link;
	compat_stack_t	uc_stack;
	compat_sigset_t	uc_sigmask;
	/* glibc uses a 1024-bit sigset_t */
	__u8		__unused[1024 / 8 - sizeof(compat_sigset_t)];
	/* last for future expansion */
	struct sigcontext uc_mcontext;
};

/*
 * Do a signal return; undo the signal stack. These are aligned to 128-bit.
 */
struct rt_sigframe {
	struct compat_siginfo info;
	struct ilp32_ucontext uc;
};

struct frame_record {
	u64 fp;
	u64 lr;
};

struct rt_sigframe_user_layout {
	void __user *sigframe;
	struct frame_record __user *next_frame;

	unsigned long size;	/* size of allocated sigframe data */
	unsigned long limit;	/* largest allowed size */

	unsigned long fpsimd_offset;
	unsigned long esr_offset;
	unsigned long sve_offset;
	unsigned long extra_offset;
	unsigned long end_offset;
};

static int get_sigset(sigset_t *set, const compat_sigset_t __user *mask)
{
	compat_sigset_t set32;

	BUILD_BUG_ON(sizeof(compat_sigset_t) != sizeof(sigset_t));

	if (copy_from_user(&set32, mask, sizeof(set32)))
		return -EFAULT;

	set->sig[0] = set32.sig[0] | (((long)set32.sig[1]) << 32);
	return 0;
}

static int put_sigset(const sigset_t *set, compat_sigset_t __user *mask)
{
	compat_sigset_t set32;

	BUILD_BUG_ON(sizeof(compat_sigset_t) != sizeof(sigset_t));

	set32.sig[0] = set->sig[0] & 0xffffffffull;
	set32.sig[1] = set->sig[0] >> 32;

	return copy_to_user(mask, &set32, sizeof(set32));
}

#define BASE_SIGFRAME_SIZE round_up(sizeof(struct rt_sigframe), 16)
#define TERMINATOR_SIZE round_up(sizeof(struct _aarch64_ctx), 16)
#define EXTRA_CONTEXT_SIZE round_up(sizeof(struct extra_context), 16)
#define SIGCONTEXT_RESERVED_SIZE sizeof(((struct sigcontext *)0)->__reserved)
#define RT_SIGFRAME_RESERVED_OFFSET offsetof(struct rt_sigframe, uc.uc_mcontext.__reserved)

static void init_user_layout(struct rt_sigframe_user_layout *user)
{
	memset(user, 0, sizeof(*user));
	user->size = RT_SIGFRAME_RESERVED_OFFSET;

	user->limit = user->size + SIGCONTEXT_RESERVED_SIZE;

	user->limit -= TERMINATOR_SIZE;
	user->limit -= EXTRA_CONTEXT_SIZE;
	/* Reserve space for extension and terminator ^ */
}

static size_t sigframe_size(struct rt_sigframe_user_layout const *user)
{
	return round_up(max(user->size, sizeof(struct rt_sigframe)), 16);
}

/*
 * Sanity limit on the approximate maximum size of signal frame we'll
 * try to generate.  Stack alignment padding and the frame record are
 * not taken into account.  This limit is not a guarantee and is
 * NOT ABI.
 */
#define SIGFRAME_MAXSZ SZ_64K

static int __sigframe_alloc(struct rt_sigframe_user_layout *user,
			    unsigned long *offset, size_t size, bool extend)
{
	size_t padded_size = round_up(size, 16);

	if (padded_size > user->limit - user->size &&
	    !user->extra_offset &&
	    extend) {
		int ret;

		user->limit += EXTRA_CONTEXT_SIZE;
		ret = __sigframe_alloc(user, &user->extra_offset,
				       sizeof(struct extra_context), false);
		if (ret) {
			user->limit -= EXTRA_CONTEXT_SIZE;
			return ret;
		}

		/* Reserve space for the __reserved[] terminator */
		user->size += TERMINATOR_SIZE;

		/*
		 * Allow expansion up to SIGFRAME_MAXSZ, ensuring space for
		 * the terminator:
		 */
		user->limit = SIGFRAME_MAXSZ - TERMINATOR_SIZE;
	}

	/* Still not enough space?  Bad luck! */
	if (padded_size > user->limit - user->size)
		return -ENOMEM;

	*offset = user->size;
	user->size += padded_size;

	return 0;
}

/*
 * Allocate space for an optional record of <size> bytes in the user
 * signal frame.  The offset from the signal frame base address to the
 * allocated block is assigned to *offset.
 */
static int sigframe_alloc(struct rt_sigframe_user_layout *user,
			  unsigned long *offset, size_t size)
{
	return __sigframe_alloc(user, offset, size, true);
}

/* Allocate the null terminator record and prevent further allocations */
static int sigframe_alloc_end(struct rt_sigframe_user_layout *user)
{
	int ret;

	/* Un-reserve the space reserved for the terminator: */
	user->limit += TERMINATOR_SIZE;

	ret = sigframe_alloc(user, &user->end_offset,
			     sizeof(struct _aarch64_ctx));
	if (ret)
		return ret;

	/* Prevent further allocation: */
	user->limit = user->size;
	return 0;
}

static void __user *apply_user_offset(
	struct rt_sigframe_user_layout const *user, unsigned long offset)
{
	char __user *base = (char __user *)user->sigframe;

	return base + offset;
}

static int preserve_fpsimd_context(struct fpsimd_context __user *ctx)
{
	struct user_fpsimd_state const *fpsimd =
		&current->thread.uw.fpsimd_state;
	int err;

	/* copy the FP and status/control registers */
	err = __copy_to_user(ctx->vregs, fpsimd->vregs, sizeof(fpsimd->vregs));
	__put_user_error(fpsimd->fpsr, &ctx->fpsr, err);
	__put_user_error(fpsimd->fpcr, &ctx->fpcr, err);

	/* copy the magic/size information */
	__put_user_error(FPSIMD_MAGIC, &ctx->head.magic, err);
	__put_user_error(sizeof(struct fpsimd_context), &ctx->head.size, err);

	return err ? -EFAULT : 0;
}

static int restore_fpsimd_context(struct fpsimd_context __user *ctx)
{
	struct user_fpsimd_state fpsimd;
	__u32 magic, size;
	int err = 0;

	/* check the magic/size information */
	__get_user_error(magic, &ctx->head.magic, err);
	__get_user_error(size, &ctx->head.size, err);
	if (err)
		return -EFAULT;
	if (magic != FPSIMD_MAGIC || size != sizeof(struct fpsimd_context))
		return -EINVAL;

	/* copy the FP and status/control registers */
	err = __copy_from_user(fpsimd.vregs, ctx->vregs,
			       sizeof(fpsimd.vregs));
	__get_user_error(fpsimd.fpsr, &ctx->fpsr, err);
	__get_user_error(fpsimd.fpcr, &ctx->fpcr, err);

	clear_thread_flag(TIF_SVE);

	/* load the hardware registers from the fpsimd_state structure */
	if (!err)
		fpsimd_update_current_state(&fpsimd);

	return err ? -EFAULT : 0;
}


struct user_ctxs {
	struct fpsimd_context __user *fpsimd;
	struct sve_context __user *sve;
};

#ifdef CONFIG_ARM64_SVE

static int preserve_sve_context(struct sve_context __user *ctx)
{
	int err = 0;
	u16 reserved[ARRAY_SIZE(ctx->__reserved)];
	unsigned int vl = current->thread.sve_vl;
	unsigned int vq = 0;

	if (test_thread_flag(TIF_SVE))
		vq = sve_vq_from_vl(vl);

	memset(reserved, 0, sizeof(reserved));

	__put_user_error(SVE_MAGIC, &ctx->head.magic, err);
	__put_user_error(round_up(SVE_SIG_CONTEXT_SIZE(vq), 16),
			 &ctx->head.size, err);
	__put_user_error(vl, &ctx->vl, err);
	BUILD_BUG_ON(sizeof(ctx->__reserved) != sizeof(reserved));
	err |= __copy_to_user(&ctx->__reserved, reserved, sizeof(reserved));

	if (vq) {
		/*
		 * This assumes that the SVE state has already been saved to
		 * the task struct by calling preserve_fpsimd_context().
		 */
		err |= __copy_to_user((char __user *)ctx + SVE_SIG_REGS_OFFSET,
				      current->thread.sve_state,
				      SVE_SIG_REGS_SIZE(vq));
	}

	return err ? -EFAULT : 0;
}

static int restore_sve_fpsimd_context(struct user_ctxs *user)
{
	int err;
	unsigned int vq;
	struct user_fpsimd_state fpsimd;
	struct sve_context sve;

	if (__copy_from_user(&sve, user->sve, sizeof(sve)))
		return -EFAULT;

	if (sve.vl != current->thread.sve_vl)
		return -EINVAL;

	if (sve.head.size <= sizeof(*user->sve)) {
		clear_thread_flag(TIF_SVE);
		goto fpsimd_only;
	}

	vq = sve_vq_from_vl(sve.vl);

	if (sve.head.size < SVE_SIG_CONTEXT_SIZE(vq))
		return -EINVAL;

	/*
	 * Careful: we are about __copy_from_user() directly into
	 * thread.sve_state with preemption enabled, so protection is
	 * needed to prevent a racing context switch from writing stale
	 * registers back over the new data.
	 */

	fpsimd_flush_task_state(current);
	/* From now, fpsimd_thread_switch() won't touch thread.sve_state */

	sve_alloc(current);
	err = __copy_from_user(current->thread.sve_state,
			       (char __user const *)user->sve +
					SVE_SIG_REGS_OFFSET,
			       SVE_SIG_REGS_SIZE(vq));
	if (err)
		return -EFAULT;

	set_thread_flag(TIF_SVE);

fpsimd_only:
	/* copy the FP and status/control registers */
	/* restore_sigframe() already checked that user->fpsimd != NULL. */
	err = __copy_from_user(fpsimd.vregs, user->fpsimd->vregs,
			       sizeof(fpsimd.vregs));
	__get_user_error(fpsimd.fpsr, &user->fpsimd->fpsr, err);
	__get_user_error(fpsimd.fpcr, &user->fpsimd->fpcr, err);

	/* load the hardware registers from the fpsimd_state structure */
	if (!err)
		fpsimd_update_current_state(&fpsimd);

	return err ? -EFAULT : 0;
}

#else /* ! CONFIG_ARM64_SVE */

/* Turn any non-optimised out attempts to use these into a link error: */
extern int preserve_sve_context(void __user *ctx);
extern int restore_sve_fpsimd_context(struct user_ctxs *user);

#endif /* ! CONFIG_ARM64_SVE */


static int parse_user_sigframe(struct user_ctxs *user,
			       struct rt_sigframe __user *sf)
{
	struct sigcontext __user *const sc = &sf->uc.uc_mcontext;
	struct _aarch64_ctx __user *head;
	char __user *base = (char __user *)&sc->__reserved;
	size_t offset = 0;
	size_t limit = sizeof(sc->__reserved);
	bool have_extra_context = false;
	char const __user *const sfp = (char const __user *)sf;

	user->fpsimd = NULL;
	user->sve = NULL;

	if (!IS_ALIGNED((unsigned long)base, 16))
		goto invalid;

	while (1) {
		int err = 0;
		u32 magic, size;
		char const __user *userp;
		struct extra_context const __user *extra;
		u64 extra_datap;
		u32 extra_size;
		struct _aarch64_ctx const __user *end;
		u32 end_magic, end_size;

		if (limit - offset < sizeof(*head))
			goto invalid;

		if (!IS_ALIGNED(offset, 16))
			goto invalid;

		head = (struct _aarch64_ctx __user *)(base + offset);
		__get_user_error(magic, &head->magic, err);
		__get_user_error(size, &head->size, err);
		if (err)
			return err;

		if (limit - offset < size)
			goto invalid;

		switch (magic) {
		case 0:
			if (size)
				goto invalid;

			goto done;

		case FPSIMD_MAGIC:
			if (user->fpsimd)
				goto invalid;

			if (size < sizeof(*user->fpsimd))
				goto invalid;

			user->fpsimd = (struct fpsimd_context __user *)head;
			break;

		case ESR_MAGIC:
			/* ignore */
			break;

		case SVE_MAGIC:
			if (!system_supports_sve())
				goto invalid;

			if (user->sve)
				goto invalid;

			if (size < sizeof(*user->sve))
				goto invalid;

			user->sve = (struct sve_context __user *)head;
			break;

		case EXTRA_MAGIC:
			if (have_extra_context)
				goto invalid;

			if (size < sizeof(*extra))
				goto invalid;

			userp = (char const __user *)head;

			extra = (struct extra_context const __user *)userp;
			userp += size;

			__get_user_error(extra_datap, &extra->datap, err);
			__get_user_error(extra_size, &extra->size, err);
			if (err)
				return err;

			/* Check for the dummy terminator in __reserved[]: */

			if (limit - offset - size < TERMINATOR_SIZE)
				goto invalid;

			end = (struct _aarch64_ctx const __user *)userp;
			userp += TERMINATOR_SIZE;

			__get_user_error(end_magic, &end->magic, err);
			__get_user_error(end_size, &end->size, err);
			if (err)
				return err;

			if (end_magic || end_size)
				goto invalid;

			/* Prevent looping/repeated parsing of extra_context */
			have_extra_context = true;

			base = (__force void __user *)extra_datap;
			if (!IS_ALIGNED((unsigned long)base, 16))
				goto invalid;

			if (!IS_ALIGNED(extra_size, 16))
				goto invalid;

			if (base != userp)
				goto invalid;

			/* Reject "unreasonably large" frames: */
			if (extra_size > sfp + SIGFRAME_MAXSZ - userp)
				goto invalid;

			/*
			 * Ignore trailing terminator in __reserved[]
			 * and start parsing extra data:
			 */
			offset = 0;
			limit = extra_size;

			if (!access_ok(base, limit))
				goto invalid;

			continue;

		default:
			goto invalid;
		}

		if (size < sizeof(*head))
			goto invalid;

		if (limit - offset < size)
			goto invalid;

		offset += size;
	}

done:
	return 0;

invalid:
	return -EINVAL;
}

static int restore_sigframe(struct pt_regs *regs,
			    struct rt_sigframe __user *sf)
{
	sigset_t set;
	int i, err;
	struct user_ctxs user;

	err = get_sigset(&set, &sf->uc.uc_sigmask);
	if (err == 0)
		set_current_blocked(&set);

	for (i = 0; i < 31; i++)
		__get_user_error(regs->regs[i], &sf->uc.uc_mcontext.regs[i],
				 err);
	__get_user_error(regs->sp, &sf->uc.uc_mcontext.sp, err);
	__get_user_error(regs->pc, &sf->uc.uc_mcontext.pc, err);
	__get_user_error(regs->pstate, &sf->uc.uc_mcontext.pstate, err);

	/*
	 * Avoid sys_rt_sigreturn() restarting.
	 */
	forget_syscall(regs);

	err |= !valid_user_regs(&regs->user_regs, current);
	if (err == 0)
		err = parse_user_sigframe(&user, sf);

	if (err == 0) {
		if (!user.fpsimd)
			return -EINVAL;

		if (user.sve) {
			if (!system_supports_sve())
				return -EINVAL;

			err = restore_sve_fpsimd_context(&user);
		} else {
			err = restore_fpsimd_context(user.fpsimd);
		}
	}

	return err;
}

COMPAT_SYSCALL_DEFINE0(ilp32_rt_sigreturn)
{
	struct pt_regs *regs = current_pt_regs();
	struct rt_sigframe __user *frame;

	/* Always make any pending restarted system calls return -EINTR */
	current->restart_block.fn = do_no_restart_syscall;

	/*
	 * Since we stacked the signal on a 128-bit boundary, then 'sp' should
	 * be word aligned here.
	 */
	if (regs->sp & 15)
		goto badframe;

	frame = (struct rt_sigframe __user *)regs->sp;

	if (!access_ok(frame, sizeof (*frame)))
		goto badframe;

	if (restore_sigframe(regs, frame))
		goto badframe;

	if (compat_restore_altstack(&frame->uc.uc_stack))
		goto badframe;

	return regs->regs[0];

badframe:
	arm64_notify_segfault(regs->sp);
	return 0;
}

/*
 * Determine the layout of optional records in the signal frame
 *
 * add_all: if true, lays out the biggest possible signal frame for
 *	this task; otherwise, generates a layout for the current state
 *	of the task.
 */
static int setup_sigframe_layout(struct rt_sigframe_user_layout *user,
				 bool add_all)
{
	int err;

	err = sigframe_alloc(user, &user->fpsimd_offset,
			     sizeof(struct fpsimd_context));
	if (err)
		return err;

	/* fault information, if valid */
	if (add_all || current->thread.fault_code) {
		err = sigframe_alloc(user, &user->esr_offset,
				     sizeof(struct esr_context));
		if (err)
			return err;
	}

	if (system_supports_sve()) {
		unsigned int vq = 0;

		if (add_all || test_thread_flag(TIF_SVE)) {
			int vl = sve_max_vl;

			if (!add_all)
				vl = current->thread.sve_vl;

			vq = sve_vq_from_vl(vl);
		}

		err = sigframe_alloc(user, &user->sve_offset,
				     SVE_SIG_CONTEXT_SIZE(vq));
		if (err)
			return err;
	}

	return sigframe_alloc_end(user);
}

static int setup_sigframe(struct rt_sigframe_user_layout *user,
			  struct pt_regs *regs, sigset_t *set)
{
	int i, err = 0;
	struct rt_sigframe __user *sf = user->sigframe;

	/* set up the stack frame for unwinding */
	__put_user_error(regs->regs[29], &user->next_frame->fp, err);
	__put_user_error(regs->regs[30], &user->next_frame->lr, err);

	for (i = 0; i < 31; i++)
		__put_user_error(regs->regs[i], &sf->uc.uc_mcontext.regs[i],
				 err);
	__put_user_error(regs->sp, &sf->uc.uc_mcontext.sp, err);
	__put_user_error(regs->pc, &sf->uc.uc_mcontext.pc, err);
	__put_user_error(regs->pstate, &sf->uc.uc_mcontext.pstate, err);

	__put_user_error(current->thread.fault_address, &sf->uc.uc_mcontext.fault_address, err);

	err |= put_sigset(set, &sf->uc.uc_sigmask);

	if (err == 0) {
		struct fpsimd_context __user *fpsimd_ctx =
			apply_user_offset(user, user->fpsimd_offset);
		err |= preserve_fpsimd_context(fpsimd_ctx);
	}

	/* fault information, if valid */
	if (err == 0 && user->esr_offset) {
		struct esr_context __user *esr_ctx =
			apply_user_offset(user, user->esr_offset);

		__put_user_error(ESR_MAGIC, &esr_ctx->head.magic, err);
		__put_user_error(sizeof(*esr_ctx), &esr_ctx->head.size, err);
		__put_user_error(current->thread.fault_code, &esr_ctx->esr, err);
	}

	/* Scalable Vector Extension state, if present */
	if (system_supports_sve() && err == 0 && user->sve_offset) {
		struct sve_context __user *sve_ctx =
			apply_user_offset(user, user->sve_offset);
		err |= preserve_sve_context(sve_ctx);
	}

	if (err == 0 && user->extra_offset) {
		char __user *sfp = (char __user *)user->sigframe;
		char __user *userp =
			apply_user_offset(user, user->extra_offset);

		struct extra_context __user *extra;
		struct _aarch64_ctx __user *end;
		u64 extra_datap;
		u32 extra_size;

		extra = (struct extra_context __user *)userp;
		userp += EXTRA_CONTEXT_SIZE;

		end = (struct _aarch64_ctx __user *)userp;
		userp += TERMINATOR_SIZE;

		/*
		 * extra_datap is just written to the signal frame.
		 * The value gets cast back to a void __user *
		 * during sigreturn.
		 */
		extra_datap = (__force u64)userp;
		extra_size = sfp + round_up(user->size, 16) - userp;

		__put_user_error(EXTRA_MAGIC, &extra->head.magic, err);
		__put_user_error(EXTRA_CONTEXT_SIZE, &extra->head.size, err);
		__put_user_error(extra_datap, &extra->datap, err);
		__put_user_error(extra_size, &extra->size, err);

		/* Add the terminator */
		__put_user_error(0, &end->magic, err);
		__put_user_error(0, &end->size, err);
	}

	/* set the "end" magic */
	if (err == 0) {
		struct _aarch64_ctx __user *end =
			apply_user_offset(user, user->end_offset);

		__put_user_error(0, &end->magic, err);
		__put_user_error(0, &end->size, err);
	}

	return err;
}

static int get_sigframe(struct rt_sigframe_user_layout *user,
			 struct ksignal *ksig, struct pt_regs *regs)
{
	unsigned long sp, sp_top;
	int err;

	init_user_layout(user);
	err = setup_sigframe_layout(user, false);
	if (err)
		return err;

	sp = sp_top = sigsp(regs->sp, ksig);

	sp = round_down(sp - sizeof(struct frame_record), 16);
	user->next_frame = (struct frame_record __user *)sp;

	sp = round_down(sp, 16) - sigframe_size(user);
	user->sigframe = (void __user *)sp;

	/*
	 * Check that we can actually write to the signal frame.
	 */
	if (!access_ok(user->sigframe, sp_top - sp))
		return -EFAULT;

	return 0;
}

static void setup_return(struct pt_regs *regs, struct k_sigaction *ka,
			 struct rt_sigframe_user_layout *user, int usig)
{
	__sigrestore_t sigtramp;

	regs->regs[0] = usig;
	regs->sp = (unsigned long)user->sigframe;
	regs->regs[29] = (unsigned long)&user->next_frame->fp;
	regs->pc = (unsigned long)ka->sa.sa_handler;

	// FIXME: What's needed here?
	if (ka->sa.sa_flags & SA_RESTORER) {
		BUG();
		sigtramp = ka->sa.sa_restorer;
	} else
		sigtramp = VDSO_SYMBOL(current->mm->context.vdso, sigtramp_ilp32);

	regs->regs[30] = (unsigned long)sigtramp;
}

int ilp32_setup_rt_frame(int usig, struct ksignal *ksig, sigset_t *set,
			  struct pt_regs *regs)
{
	struct rt_sigframe_user_layout user;
	struct rt_sigframe __user *frame;
	int err = 0;

	fpsimd_signal_preserve_current_state();

	if (get_sigframe(&user, ksig, regs))
		return 1;

	frame = user.sigframe;

	__put_user_error(0, &frame->uc.uc_flags, err);
	__put_user_error((typeof(frame->uc.uc_link)) 0, &frame->uc.uc_link, err);

	err |= __compat_save_altstack(&frame->uc.uc_stack, regs->sp);
	err |= setup_sigframe(&user, regs, set);
	if (err == 0) {
		setup_return(regs, &ksig->ka, &user, usig);
		if (ksig->ka.sa.sa_flags & SA_SIGINFO) {
			err |= copy_siginfo_to_user32(&frame->info, &ksig->info);
			regs->regs[1] = (unsigned long)&frame->info;
			regs->regs[2] = (unsigned long)&frame->uc;
		}
	}

	return err;
}
