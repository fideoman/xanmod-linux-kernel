/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Read-Copy Update mechanism for mutual exclusion (tree-based version)
 * Internal non-public definitions that provide either classic
 * or preemptible semantics.
 *
 * Copyright Red Hat, 2009
 * Copyright IBM Corporation, 2009
 *
 * Author: Ingo Molnar <mingo@elte.hu>
 *	   Paul E. McKenney <paulmck@linux.ibm.com>
 */

#include "../locking/rtmutex_common.h"

#ifdef CONFIG_RCU_NOCB_CPU
static cpumask_var_t rcu_nocb_mask; /* CPUs to have callbacks offloaded. */
static bool __read_mostly rcu_nocb_poll;    /* Offload kthread are to poll. */
#endif /* #ifdef CONFIG_RCU_NOCB_CPU */

/*
 * Check the RCU kernel configuration parameters and print informative
 * messages about anything out of the ordinary.
 */
static void __init rcu_bootup_announce_oddness(void)
{
	if (IS_ENABLED(CONFIG_RCU_TRACE))
		pr_info("\tRCU event tracing is enabled.\n");
	if ((IS_ENABLED(CONFIG_64BIT) && RCU_FANOUT != 64) ||
	    (!IS_ENABLED(CONFIG_64BIT) && RCU_FANOUT != 32))
		pr_info("\tCONFIG_RCU_FANOUT set to non-default value of %d.\n",
			RCU_FANOUT);
	if (rcu_fanout_exact)
		pr_info("\tHierarchical RCU autobalancing is disabled.\n");
	if (IS_ENABLED(CONFIG_RCU_FAST_NO_HZ))
		pr_info("\tRCU dyntick-idle grace-period acceleration is enabled.\n");
	if (IS_ENABLED(CONFIG_PROVE_RCU))
		pr_info("\tRCU lockdep checking is enabled.\n");
	if (RCU_NUM_LVLS >= 4)
		pr_info("\tFour(or more)-level hierarchy is enabled.\n");
	if (RCU_FANOUT_LEAF != 16)
		pr_info("\tBuild-time adjustment of leaf fanout to %d.\n",
			RCU_FANOUT_LEAF);
	if (rcu_fanout_leaf != RCU_FANOUT_LEAF)
		pr_info("\tBoot-time adjustment of leaf fanout to %d.\n",
			rcu_fanout_leaf);
	if (nr_cpu_ids != NR_CPUS)
		pr_info("\tRCU restricting CPUs from NR_CPUS=%d to nr_cpu_ids=%u.\n", NR_CPUS, nr_cpu_ids);
#ifdef CONFIG_RCU_BOOST
	pr_info("\tRCU priority boosting: priority %d delay %d ms.\n",
		kthread_prio, CONFIG_RCU_BOOST_DELAY);
#endif
	if (blimit != DEFAULT_RCU_BLIMIT)
		pr_info("\tBoot-time adjustment of callback invocation limit to %ld.\n", blimit);
	if (qhimark != DEFAULT_RCU_QHIMARK)
		pr_info("\tBoot-time adjustment of callback high-water mark to %ld.\n", qhimark);
	if (qlowmark != DEFAULT_RCU_QLOMARK)
		pr_info("\tBoot-time adjustment of callback low-water mark to %ld.\n", qlowmark);
	if (jiffies_till_first_fqs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of first FQS scan delay to %ld jiffies.\n", jiffies_till_first_fqs);
	if (jiffies_till_next_fqs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of subsequent FQS scan delay to %ld jiffies.\n", jiffies_till_next_fqs);
	if (jiffies_till_sched_qs != ULONG_MAX)
		pr_info("\tBoot-time adjustment of scheduler-enlistment delay to %ld jiffies.\n", jiffies_till_sched_qs);
	if (rcu_kick_kthreads)
		pr_info("\tKick kthreads if too-long grace period.\n");
	if (IS_ENABLED(CONFIG_DEBUG_OBJECTS_RCU_HEAD))
		pr_info("\tRCU callback double-/use-after-free debug enabled.\n");
	if (gp_preinit_delay)
		pr_info("\tRCU debug GP pre-init slowdown %d jiffies.\n", gp_preinit_delay);
	if (gp_init_delay)
		pr_info("\tRCU debug GP init slowdown %d jiffies.\n", gp_init_delay);
	if (gp_cleanup_delay)
		pr_info("\tRCU debug GP init slowdown %d jiffies.\n", gp_cleanup_delay);
	if (!use_softirq)
		pr_info("\tRCU_SOFTIRQ processing moved to rcuc kthreads.\n");
	if (IS_ENABLED(CONFIG_RCU_EQS_DEBUG))
		pr_info("\tRCU debug extended QS entry/exit.\n");
	rcupdate_announce_bootup_oddness();
}

#ifdef CONFIG_PREEMPT_RCU

static void rcu_report_exp_rnp(struct rcu_node *rnp, bool wake);
static void rcu_read_unlock_special(struct task_struct *t);

/*
 * Tell them what RCU they are running.
 */
static void __init rcu_bootup_announce(void)
{
	pr_info("Preemptible hierarchical RCU implementation.\n");
	rcu_bootup_announce_oddness();
}

/* Flags for rcu_preempt_ctxt_queue() decision table. */
#define RCU_GP_TASKS	0x8
#define RCU_EXP_TASKS	0x4
#define RCU_GP_BLKD	0x2
#define RCU_EXP_BLKD	0x1

/*
 * Queues a task preempted within an RCU-preempt read-side critical
 * section into the appropriate location within the ->blkd_tasks list,
 * depending on the states of any ongoing normal and expedited grace
 * periods.  The ->gp_tasks pointer indicates which element the normal
 * grace period is waiting on (NULL if none), and the ->exp_tasks pointer
 * indicates which element the expedited grace period is waiting on (again,
 * NULL if none).  If a grace period is waiting on a given element in the
 * ->blkd_tasks list, it also waits on all subsequent elements.  Thus,
 * adding a task to the tail of the list blocks any grace period that is
 * already waiting on one of the elements.  In contrast, adding a task
 * to the head of the list won't block any grace period that is already
 * waiting on one of the elements.
 *
 * This queuing is imprecise, and can sometimes make an ongoing grace
 * period wait for a task that is not strictly speaking blocking it.
 * Given the choice, we needlessly block a normal grace period rather than
 * blocking an expedited grace period.
 *
 * Note that an endless sequence of expedited grace periods still cannot
 * indefinitely postpone a normal grace period.  Eventually, all of the
 * fixed number of preempted tasks blocking the normal grace period that are
 * not also blocking the expedited grace period will resume and complete
 * their RCU read-side critical sections.  At that point, the ->gp_tasks
 * pointer will equal the ->exp_tasks pointer, at which point the end of
 * the corresponding expedited grace period will also be the end of the
 * normal grace period.
 */
static void rcu_preempt_ctxt_queue(struct rcu_node *rnp, struct rcu_data *rdp)
	__releases(rnp->lock) /* But leaves rrupts disabled. */
{
	int blkd_state = (rnp->gp_tasks ? RCU_GP_TASKS : 0) +
			 (rnp->exp_tasks ? RCU_EXP_TASKS : 0) +
			 (rnp->qsmask & rdp->grpmask ? RCU_GP_BLKD : 0) +
			 (rnp->expmask & rdp->grpmask ? RCU_EXP_BLKD : 0);
	struct task_struct *t = current;

	raw_lockdep_assert_held_rcu_node(rnp);
	WARN_ON_ONCE(rdp->mynode != rnp);
	WARN_ON_ONCE(!rcu_is_leaf_node(rnp));
	/* RCU better not be waiting on newly onlined CPUs! */
	WARN_ON_ONCE(rnp->qsmaskinitnext & ~rnp->qsmaskinit & rnp->qsmask &
		     rdp->grpmask);

	/*
	 * Decide where to queue the newly blocked task.  In theory,
	 * this could be an if-statement.  In practice, when I tried
	 * that, it was quite messy.
	 */
	switch (blkd_state) {
	case 0:
	case                RCU_EXP_TASKS:
	case                RCU_EXP_TASKS + RCU_GP_BLKD:
	case RCU_GP_TASKS:
	case RCU_GP_TASKS + RCU_EXP_TASKS:

		/*
		 * Blocking neither GP, or first task blocking the normal
		 * GP but not blocking the already-waiting expedited GP.
		 * Queue at the head of the list to avoid unnecessarily
		 * blocking the already-waiting GPs.
		 */
		list_add(&t->rcu_node_entry, &rnp->blkd_tasks);
		break;

	case                                              RCU_EXP_BLKD:
	case                                RCU_GP_BLKD:
	case                                RCU_GP_BLKD + RCU_EXP_BLKD:
	case RCU_GP_TASKS +                               RCU_EXP_BLKD:
	case RCU_GP_TASKS +                 RCU_GP_BLKD + RCU_EXP_BLKD:
	case RCU_GP_TASKS + RCU_EXP_TASKS + RCU_GP_BLKD + RCU_EXP_BLKD:

		/*
		 * First task arriving that blocks either GP, or first task
		 * arriving that blocks the expedited GP (with the normal
		 * GP already waiting), or a task arriving that blocks
		 * both GPs with both GPs already waiting.  Queue at the
		 * tail of the list to avoid any GP waiting on any of the
		 * already queued tasks that are not blocking it.
		 */
		list_add_tail(&t->rcu_node_entry, &rnp->blkd_tasks);
		break;

	case                RCU_EXP_TASKS +               RCU_EXP_BLKD:
	case                RCU_EXP_TASKS + RCU_GP_BLKD + RCU_EXP_BLKD:
	case RCU_GP_TASKS + RCU_EXP_TASKS +               RCU_EXP_BLKD:

		/*
		 * Second or subsequent task blocking the expedited GP.
		 * The task either does not block the normal GP, or is the
		 * first task blocking the normal GP.  Queue just after
		 * the first task blocking the expedited GP.
		 */
		list_add(&t->rcu_node_entry, rnp->exp_tasks);
		break;

	case RCU_GP_TASKS +                 RCU_GP_BLKD:
	case RCU_GP_TASKS + RCU_EXP_TASKS + RCU_GP_BLKD:

		/*
		 * Second or subsequent task blocking the normal GP.
		 * The task does not block the expedited GP. Queue just
		 * after the first task blocking the normal GP.
		 */
		list_add(&t->rcu_node_entry, rnp->gp_tasks);
		break;

	default:

		/* Yet another exercise in excessive paranoia. */
		WARN_ON_ONCE(1);
		break;
	}

	/*
	 * We have now queued the task.  If it was the first one to
	 * block either grace period, update the ->gp_tasks and/or
	 * ->exp_tasks pointers, respectively, to reference the newly
	 * blocked tasks.
	 */
	if (!rnp->gp_tasks && (blkd_state & RCU_GP_BLKD)) {
		rnp->gp_tasks = &t->rcu_node_entry;
		WARN_ON_ONCE(rnp->completedqs == rnp->gp_seq);
	}
	if (!rnp->exp_tasks && (blkd_state & RCU_EXP_BLKD))
		rnp->exp_tasks = &t->rcu_node_entry;
	WARN_ON_ONCE(!(blkd_state & RCU_GP_BLKD) !=
		     !(rnp->qsmask & rdp->grpmask));
	WARN_ON_ONCE(!(blkd_state & RCU_EXP_BLKD) !=
		     !(rnp->expmask & rdp->grpmask));
	raw_spin_unlock_rcu_node(rnp); /* interrupts remain disabled. */

	/*
	 * Report the quiescent state for the expedited GP.  This expedited
	 * GP should not be able to end until we report, so there should be
	 * no need to check for a subsequent expedited GP.  (Though we are
	 * still in a quiescent state in any case.)
	 */
	if (blkd_state & RCU_EXP_BLKD && rdp->deferred_qs)
		rcu_report_exp_rdp(rdp);
	else
		WARN_ON_ONCE(rdp->deferred_qs);
}

/*
 * Record a preemptible-RCU quiescent state for the specified CPU.
 * Note that this does not necessarily mean that the task currently running
 * on the CPU is in a quiescent state:  Instead, it means that the current
 * grace period need not wait on any RCU read-side critical section that
 * starts later on this CPU.  It also means that if the current task is
 * in an RCU read-side critical section, it has already added itself to
 * some leaf rcu_node structure's ->blkd_tasks list.  In addition to the
 * current task, there might be any number of other tasks blocked while
 * in an RCU read-side critical section.
 *
 * Callers to this function must disable preemption.
 */
static void rcu_qs(void)
{
	RCU_LOCKDEP_WARN(preemptible(), "rcu_qs() invoked with preemption enabled!!!\n");
	if (__this_cpu_read(rcu_data.cpu_no_qs.s)) {
		trace_rcu_grace_period(TPS("rcu_preempt"),
				       __this_cpu_read(rcu_data.gp_seq),
				       TPS("cpuqs"));
		__this_cpu_write(rcu_data.cpu_no_qs.b.norm, false);
		barrier(); /* Coordinate with rcu_flavor_sched_clock_irq(). */
		WRITE_ONCE(current->rcu_read_unlock_special.b.need_qs, false);
	}
}

/*
 * We have entered the scheduler, and the current task might soon be
 * context-switched away from.  If this task is in an RCU read-side
 * critical section, we will no longer be able to rely on the CPU to
 * record that fact, so we enqueue the task on the blkd_tasks list.
 * The task will dequeue itself when it exits the outermost enclosing
 * RCU read-side critical section.  Therefore, the current grace period
 * cannot be permitted to complete until the blkd_tasks list entries
 * predating the current grace period drain, in other words, until
 * rnp->gp_tasks becomes NULL.
 *
 * Caller must disable interrupts.
 */
void rcu_note_context_switch(bool preempt)
{
	struct task_struct *t = current;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	struct rcu_node *rnp;
	int sleeping_l = 0;

	barrier(); /* Avoid RCU read-side critical sections leaking down. */
	trace_rcu_utilization(TPS("Start context switch"));
	lockdep_assert_irqs_disabled();
#if defined(CONFIG_PREEMPT_RT_FULL)
	sleeping_l = t->sleeping_lock;
#endif
	WARN_ON_ONCE(!preempt && t->rcu_read_lock_nesting > 0 && !sleeping_l);
	if (t->rcu_read_lock_nesting > 0 &&
	    !t->rcu_read_unlock_special.b.blocked) {

		/* Possibly blocking in an RCU read-side critical section. */
		rnp = rdp->mynode;
		raw_spin_lock_rcu_node(rnp);
		t->rcu_read_unlock_special.b.blocked = true;
		t->rcu_blocked_node = rnp;

		/*
		 * Verify the CPU's sanity, trace the preemption, and
		 * then queue the task as required based on the states
		 * of any ongoing and expedited grace periods.
		 */
		WARN_ON_ONCE((rdp->grpmask & rcu_rnp_online_cpus(rnp)) == 0);
		WARN_ON_ONCE(!list_empty(&t->rcu_node_entry));
		trace_rcu_preempt_task(rcu_state.name,
				       t->pid,
				       (rnp->qsmask & rdp->grpmask)
				       ? rnp->gp_seq
				       : rcu_seq_snap(&rnp->gp_seq));
		rcu_preempt_ctxt_queue(rnp, rdp);
	} else if (t->rcu_read_lock_nesting < 0 &&
		   t->rcu_read_unlock_special.s) {

		/*
		 * Complete exit from RCU read-side critical section on
		 * behalf of preempted instance of __rcu_read_unlock().
		 */
		rcu_read_unlock_special(t);
		rcu_preempt_deferred_qs(t);
	} else {
		rcu_preempt_deferred_qs(t);
	}

	/*
	 * Either we were not in an RCU read-side critical section to
	 * begin with, or we have now recorded that critical section
	 * globally.  Either way, we can now note a quiescent state
	 * for this CPU.  Again, if we were in an RCU read-side critical
	 * section, and if that critical section was blocking the current
	 * grace period, then the fact that the task has been enqueued
	 * means that we continue to block the current grace period.
	 */
	rcu_qs();
	if (rdp->deferred_qs)
		rcu_report_exp_rdp(rdp);
	trace_rcu_utilization(TPS("End context switch"));
	barrier(); /* Avoid RCU read-side critical sections leaking up. */
}
EXPORT_SYMBOL_GPL(rcu_note_context_switch);

/*
 * Check for preempted RCU readers blocking the current grace period
 * for the specified rcu_node structure.  If the caller needs a reliable
 * answer, it must hold the rcu_node's ->lock.
 */
static int rcu_preempt_blocked_readers_cgp(struct rcu_node *rnp)
{
	return rnp->gp_tasks != NULL;
}

/* Bias and limit values for ->rcu_read_lock_nesting. */
#define RCU_NEST_BIAS INT_MAX
#define RCU_NEST_NMAX (-INT_MAX / 2)
#define RCU_NEST_PMAX (INT_MAX / 2)

/*
 * Preemptible RCU implementation for rcu_read_lock().
 * Just increment ->rcu_read_lock_nesting, shared state will be updated
 * if we block.
 */
void __rcu_read_lock(void)
{
	current->rcu_read_lock_nesting++;
	if (IS_ENABLED(CONFIG_PROVE_LOCKING))
		WARN_ON_ONCE(current->rcu_read_lock_nesting > RCU_NEST_PMAX);
	barrier();  /* critical section after entry code. */
}
EXPORT_SYMBOL_GPL(__rcu_read_lock);

/*
 * Preemptible RCU implementation for rcu_read_unlock().
 * Decrement ->rcu_read_lock_nesting.  If the result is zero (outermost
 * rcu_read_unlock()) and ->rcu_read_unlock_special is non-zero, then
 * invoke rcu_read_unlock_special() to clean up after a context switch
 * in an RCU read-side critical section and other special cases.
 */
void __rcu_read_unlock(void)
{
	struct task_struct *t = current;

	if (t->rcu_read_lock_nesting != 1) {
		--t->rcu_read_lock_nesting;
	} else {
		barrier();  /* critical section before exit code. */
		t->rcu_read_lock_nesting = -RCU_NEST_BIAS;
		barrier();  /* assign before ->rcu_read_unlock_special load */
		if (unlikely(READ_ONCE(t->rcu_read_unlock_special.s)))
			rcu_read_unlock_special(t);
		barrier();  /* ->rcu_read_unlock_special load before assign */
		t->rcu_read_lock_nesting = 0;
	}
	if (IS_ENABLED(CONFIG_PROVE_LOCKING)) {
		int rrln = t->rcu_read_lock_nesting;

		WARN_ON_ONCE(rrln < 0 && rrln > RCU_NEST_NMAX);
	}
}
EXPORT_SYMBOL_GPL(__rcu_read_unlock);

/*
 * Advance a ->blkd_tasks-list pointer to the next entry, instead
 * returning NULL if at the end of the list.
 */
static struct list_head *rcu_next_node_entry(struct task_struct *t,
					     struct rcu_node *rnp)
{
	struct list_head *np;

	np = t->rcu_node_entry.next;
	if (np == &rnp->blkd_tasks)
		np = NULL;
	return np;
}

/*
 * Return true if the specified rcu_node structure has tasks that were
 * preempted within an RCU read-side critical section.
 */
static bool rcu_preempt_has_tasks(struct rcu_node *rnp)
{
	return !list_empty(&rnp->blkd_tasks);
}

/*
 * Report deferred quiescent states.  The deferral time can
 * be quite short, for example, in the case of the call from
 * rcu_read_unlock_special().
 */
static void
rcu_preempt_deferred_qs_irqrestore(struct task_struct *t, unsigned long flags)
{
	bool empty_exp;
	bool empty_norm;
	bool empty_exp_now;
	struct list_head *np;
	bool drop_boost_mutex = false;
	struct rcu_data *rdp;
	struct rcu_node *rnp;
	union rcu_special special;

	/*
	 * If RCU core is waiting for this CPU to exit its critical section,
	 * report the fact that it has exited.  Because irqs are disabled,
	 * t->rcu_read_unlock_special cannot change.
	 */
	special = t->rcu_read_unlock_special;
	rdp = this_cpu_ptr(&rcu_data);
	if (!special.s && !rdp->deferred_qs) {
		local_irq_restore(flags);
		return;
	}
	if (special.b.need_qs) {
		rcu_qs();
		t->rcu_read_unlock_special.b.need_qs = false;
		if (!t->rcu_read_unlock_special.s && !rdp->deferred_qs) {
			local_irq_restore(flags);
			return;
		}
	}

	/*
	 * Respond to a request by an expedited grace period for a
	 * quiescent state from this CPU.  Note that requests from
	 * tasks are handled when removing the task from the
	 * blocked-tasks list below.
	 */
	if (rdp->deferred_qs) {
		rcu_report_exp_rdp(rdp);
		if (!t->rcu_read_unlock_special.s) {
			local_irq_restore(flags);
			return;
		}
	}

	/* Clean up if blocked during RCU read-side critical section. */
	if (special.b.blocked) {
		t->rcu_read_unlock_special.b.blocked = false;

		/*
		 * Remove this task from the list it blocked on.  The task
		 * now remains queued on the rcu_node corresponding to the
		 * CPU it first blocked on, so there is no longer any need
		 * to loop.  Retain a WARN_ON_ONCE() out of sheer paranoia.
		 */
		rnp = t->rcu_blocked_node;
		raw_spin_lock_rcu_node(rnp); /* irqs already disabled. */
		WARN_ON_ONCE(rnp != t->rcu_blocked_node);
		WARN_ON_ONCE(!rcu_is_leaf_node(rnp));
		empty_norm = !rcu_preempt_blocked_readers_cgp(rnp);
		WARN_ON_ONCE(rnp->completedqs == rnp->gp_seq &&
			     (!empty_norm || rnp->qsmask));
		empty_exp = sync_rcu_preempt_exp_done(rnp);
		smp_mb(); /* ensure expedited fastpath sees end of RCU c-s. */
		np = rcu_next_node_entry(t, rnp);
		list_del_init(&t->rcu_node_entry);
		t->rcu_blocked_node = NULL;
		trace_rcu_unlock_preempted_task(TPS("rcu_preempt"),
						rnp->gp_seq, t->pid);
		if (&t->rcu_node_entry == rnp->gp_tasks)
			rnp->gp_tasks = np;
		if (&t->rcu_node_entry == rnp->exp_tasks)
			rnp->exp_tasks = np;
		if (IS_ENABLED(CONFIG_RCU_BOOST)) {
			/* Snapshot ->boost_mtx ownership w/rnp->lock held. */
			drop_boost_mutex = rt_mutex_owner(&rnp->boost_mtx) == t;
			if (&t->rcu_node_entry == rnp->boost_tasks)
				rnp->boost_tasks = np;
		}

		/*
		 * If this was the last task on the current list, and if
		 * we aren't waiting on any CPUs, report the quiescent state.
		 * Note that rcu_report_unblock_qs_rnp() releases rnp->lock,
		 * so we must take a snapshot of the expedited state.
		 */
		empty_exp_now = sync_rcu_preempt_exp_done(rnp);
		if (!empty_norm && !rcu_preempt_blocked_readers_cgp(rnp)) {
			trace_rcu_quiescent_state_report(TPS("preempt_rcu"),
							 rnp->gp_seq,
							 0, rnp->qsmask,
							 rnp->level,
							 rnp->grplo,
							 rnp->grphi,
							 !!rnp->gp_tasks);
			rcu_report_unblock_qs_rnp(rnp, flags);
		} else {
			raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		}

		/* Unboost if we were boosted. */
		if (IS_ENABLED(CONFIG_RCU_BOOST) && drop_boost_mutex)
			rt_mutex_futex_unlock(&rnp->boost_mtx);

		/*
		 * If this was the last task on the expedited lists,
		 * then we need to report up the rcu_node hierarchy.
		 */
		if (!empty_exp && empty_exp_now)
			rcu_report_exp_rnp(rnp, true);
	} else {
		local_irq_restore(flags);
	}
}

/*
 * Is a deferred quiescent-state pending, and are we also not in
 * an RCU read-side critical section?  It is the caller's responsibility
 * to ensure it is otherwise safe to report any deferred quiescent
 * states.  The reason for this is that it is safe to report a
 * quiescent state during context switch even though preemption
 * is disabled.  This function cannot be expected to understand these
 * nuances, so the caller must handle them.
 */
static bool rcu_preempt_need_deferred_qs(struct task_struct *t)
{
	return (__this_cpu_read(rcu_data.deferred_qs) ||
		READ_ONCE(t->rcu_read_unlock_special.s)) &&
	       t->rcu_read_lock_nesting <= 0;
}

/*
 * Report a deferred quiescent state if needed and safe to do so.
 * As with rcu_preempt_need_deferred_qs(), "safe" involves only
 * not being in an RCU read-side critical section.  The caller must
 * evaluate safety in terms of interrupt, softirq, and preemption
 * disabling.
 */
static void rcu_preempt_deferred_qs(struct task_struct *t)
{
	unsigned long flags;
	bool couldrecurse = t->rcu_read_lock_nesting >= 0;

	if (!rcu_preempt_need_deferred_qs(t))
		return;
	if (couldrecurse)
		t->rcu_read_lock_nesting -= RCU_NEST_BIAS;
	local_irq_save(flags);
	rcu_preempt_deferred_qs_irqrestore(t, flags);
	if (couldrecurse)
		t->rcu_read_lock_nesting += RCU_NEST_BIAS;
}

/*
 * Handle special cases during rcu_read_unlock(), such as needing to
 * notify RCU core processing or task having blocked during the RCU
 * read-side critical section.
 */
static void rcu_read_unlock_special(struct task_struct *t)
{
	unsigned long flags;
	bool preempt_bh_were_disabled =
			!!(preempt_count() & (PREEMPT_MASK | SOFTIRQ_MASK));
	bool irqs_were_disabled;

	/* NMI handlers cannot block and cannot safely manipulate state. */
	if (in_nmi())
		return;

	local_irq_save(flags);
	irqs_were_disabled = irqs_disabled_flags(flags);
	if (preempt_bh_were_disabled || irqs_were_disabled) {
		WRITE_ONCE(t->rcu_read_unlock_special.b.exp_hint, false);
		/* Need to defer quiescent state until everything is enabled. */
		if (irqs_were_disabled && use_softirq) {
			/* Enabling irqs does not reschedule, so... */
			raise_softirq_irqoff(RCU_SOFTIRQ);
		} else {
			/* Enabling BH or preempt does reschedule, so... */
			set_tsk_need_resched(current);
			set_preempt_need_resched();
		}
		local_irq_restore(flags);
		return;
	}
	WRITE_ONCE(t->rcu_read_unlock_special.b.exp_hint, false);
	rcu_preempt_deferred_qs_irqrestore(t, flags);
}

/*
 * Check that the list of blocked tasks for the newly completed grace
 * period is in fact empty.  It is a serious bug to complete a grace
 * period that still has RCU readers blocked!  This function must be
 * invoked -before- updating this rnp's ->gp_seq, and the rnp's ->lock
 * must be held by the caller.
 *
 * Also, if there are blocked tasks on the list, they automatically
 * block the newly created grace period, so set up ->gp_tasks accordingly.
 */
static void rcu_preempt_check_blocked_tasks(struct rcu_node *rnp)
{
	struct task_struct *t;

	RCU_LOCKDEP_WARN(preemptible(), "rcu_preempt_check_blocked_tasks() invoked with preemption enabled!!!\n");
	if (WARN_ON_ONCE(rcu_preempt_blocked_readers_cgp(rnp)))
		dump_blkd_tasks(rnp, 10);
	if (rcu_preempt_has_tasks(rnp) &&
	    (rnp->qsmaskinit || rnp->wait_blkd_tasks)) {
		rnp->gp_tasks = rnp->blkd_tasks.next;
		t = container_of(rnp->gp_tasks, struct task_struct,
				 rcu_node_entry);
		trace_rcu_unlock_preempted_task(TPS("rcu_preempt-GPS"),
						rnp->gp_seq, t->pid);
	}
	WARN_ON_ONCE(rnp->qsmask);
}

/*
 * Check for a quiescent state from the current CPU, including voluntary
 * context switches for Tasks RCU.  When a task blocks, the task is
 * recorded in the corresponding CPU's rcu_node structure, which is checked
 * elsewhere, hence this function need only check for quiescent states
 * related to the current CPU, not to those related to tasks.
 */
static void rcu_flavor_sched_clock_irq(int user)
{
	struct task_struct *t = current;

	if (user || rcu_is_cpu_rrupt_from_idle()) {
		rcu_note_voluntary_context_switch(current);
	}
	if (t->rcu_read_lock_nesting > 0 ||
	    (preempt_count() & (PREEMPT_MASK | SOFTIRQ_MASK))) {
		/* No QS, force context switch if deferred. */
		if (rcu_preempt_need_deferred_qs(t)) {
			set_tsk_need_resched(t);
			set_preempt_need_resched();
		}
	} else if (rcu_preempt_need_deferred_qs(t)) {
		rcu_preempt_deferred_qs(t); /* Report deferred QS. */
		return;
	} else if (!t->rcu_read_lock_nesting) {
		rcu_qs(); /* Report immediate QS. */
		return;
	}

	/* If GP is oldish, ask for help from rcu_read_unlock_special(). */
	if (t->rcu_read_lock_nesting > 0 &&
	    __this_cpu_read(rcu_data.core_needs_qs) &&
	    __this_cpu_read(rcu_data.cpu_no_qs.b.norm) &&
	    !t->rcu_read_unlock_special.b.need_qs &&
	    time_after(jiffies, rcu_state.gp_start + HZ))
		t->rcu_read_unlock_special.b.need_qs = true;
}

/*
 * Check for a task exiting while in a preemptible-RCU read-side
 * critical section, clean up if so.  No need to issue warnings, as
 * debug_check_no_locks_held() already does this if lockdep is enabled.
 * Besides, if this function does anything other than just immediately
 * return, there was a bug of some sort.  Spewing warnings from this
 * function is like as not to simply obscure important prior warnings.
 */
void exit_rcu(void)
{
	struct task_struct *t = current;

	if (unlikely(!list_empty(&current->rcu_node_entry))) {
		t->rcu_read_lock_nesting = 1;
		barrier();
		WRITE_ONCE(t->rcu_read_unlock_special.b.blocked, true);
	} else if (unlikely(t->rcu_read_lock_nesting)) {
		t->rcu_read_lock_nesting = 1;
	} else {
		return;
	}
	__rcu_read_unlock();
	rcu_preempt_deferred_qs(current);
}

/*
 * Dump the blocked-tasks state, but limit the list dump to the
 * specified number of elements.
 */
static void
dump_blkd_tasks(struct rcu_node *rnp, int ncheck)
{
	int cpu;
	int i;
	struct list_head *lhp;
	bool onl;
	struct rcu_data *rdp;
	struct rcu_node *rnp1;

	raw_lockdep_assert_held_rcu_node(rnp);
	pr_info("%s: grp: %d-%d level: %d ->gp_seq %ld ->completedqs %ld\n",
		__func__, rnp->grplo, rnp->grphi, rnp->level,
		(long)rnp->gp_seq, (long)rnp->completedqs);
	for (rnp1 = rnp; rnp1; rnp1 = rnp1->parent)
		pr_info("%s: %d:%d ->qsmask %#lx ->qsmaskinit %#lx ->qsmaskinitnext %#lx\n",
			__func__, rnp1->grplo, rnp1->grphi, rnp1->qsmask, rnp1->qsmaskinit, rnp1->qsmaskinitnext);
	pr_info("%s: ->gp_tasks %p ->boost_tasks %p ->exp_tasks %p\n",
		__func__, rnp->gp_tasks, rnp->boost_tasks, rnp->exp_tasks);
	pr_info("%s: ->blkd_tasks", __func__);
	i = 0;
	list_for_each(lhp, &rnp->blkd_tasks) {
		pr_cont(" %p", lhp);
		if (++i >= 10)
			break;
	}
	pr_cont("\n");
	for (cpu = rnp->grplo; cpu <= rnp->grphi; cpu++) {
		rdp = per_cpu_ptr(&rcu_data, cpu);
		onl = !!(rdp->grpmask & rcu_rnp_online_cpus(rnp));
		pr_info("\t%d: %c online: %ld(%d) offline: %ld(%d)\n",
			cpu, ".o"[onl],
			(long)rdp->rcu_onl_gp_seq, rdp->rcu_onl_gp_flags,
			(long)rdp->rcu_ofl_gp_seq, rdp->rcu_ofl_gp_flags);
	}
}

#else /* #ifdef CONFIG_PREEMPT_RCU */

/*
 * Tell them what RCU they are running.
 */
static void __init rcu_bootup_announce(void)
{
	pr_info("Hierarchical RCU implementation.\n");
	rcu_bootup_announce_oddness();
}

/*
 * Note a quiescent state for PREEMPT=n.  Because we do not need to know
 * how many quiescent states passed, just if there was at least one since
 * the start of the grace period, this just sets a flag.  The caller must
 * have disabled preemption.
 */
static void rcu_qs(void)
{
	RCU_LOCKDEP_WARN(preemptible(), "rcu_qs() invoked with preemption enabled!!!");
	if (!__this_cpu_read(rcu_data.cpu_no_qs.s))
		return;
	trace_rcu_grace_period(TPS("rcu_sched"),
			       __this_cpu_read(rcu_data.gp_seq), TPS("cpuqs"));
	__this_cpu_write(rcu_data.cpu_no_qs.b.norm, false);
	if (!__this_cpu_read(rcu_data.cpu_no_qs.b.exp))
		return;
	__this_cpu_write(rcu_data.cpu_no_qs.b.exp, false);
	rcu_report_exp_rdp(this_cpu_ptr(&rcu_data));
}

/*
 * Register an urgently needed quiescent state.  If there is an
 * emergency, invoke rcu_momentary_dyntick_idle() to do a heavy-weight
 * dyntick-idle quiescent state visible to other CPUs, which will in
 * some cases serve for expedited as well as normal grace periods.
 * Either way, register a lightweight quiescent state.
 *
 * The barrier() calls are redundant in the common case when this is
 * called externally, but just in case this is called from within this
 * file.
 *
 */
void rcu_all_qs(void)
{
	unsigned long flags;

	if (!raw_cpu_read(rcu_data.rcu_urgent_qs))
		return;
	preempt_disable();
	/* Load rcu_urgent_qs before other flags. */
	if (!smp_load_acquire(this_cpu_ptr(&rcu_data.rcu_urgent_qs))) {
		preempt_enable();
		return;
	}
	this_cpu_write(rcu_data.rcu_urgent_qs, false);
	barrier(); /* Avoid RCU read-side critical sections leaking down. */
	if (unlikely(raw_cpu_read(rcu_data.rcu_need_heavy_qs))) {
		local_irq_save(flags);
		rcu_momentary_dyntick_idle();
		local_irq_restore(flags);
	}
	rcu_qs();
	barrier(); /* Avoid RCU read-side critical sections leaking up. */
	preempt_enable();
}
EXPORT_SYMBOL_GPL(rcu_all_qs);

/*
 * Note a PREEMPT=n context switch.  The caller must have disabled interrupts.
 */
void rcu_note_context_switch(bool preempt)
{
	barrier(); /* Avoid RCU read-side critical sections leaking down. */
	trace_rcu_utilization(TPS("Start context switch"));
	rcu_qs();
	/* Load rcu_urgent_qs before other flags. */
	if (!smp_load_acquire(this_cpu_ptr(&rcu_data.rcu_urgent_qs)))
		goto out;
	this_cpu_write(rcu_data.rcu_urgent_qs, false);
	if (unlikely(raw_cpu_read(rcu_data.rcu_need_heavy_qs)))
		rcu_momentary_dyntick_idle();
	if (!preempt)
		rcu_tasks_qs(current);
out:
	trace_rcu_utilization(TPS("End context switch"));
	barrier(); /* Avoid RCU read-side critical sections leaking up. */
}
EXPORT_SYMBOL_GPL(rcu_note_context_switch);

/*
 * Because preemptible RCU does not exist, there are never any preempted
 * RCU readers.
 */
static int rcu_preempt_blocked_readers_cgp(struct rcu_node *rnp)
{
	return 0;
}

/*
 * Because there is no preemptible RCU, there can be no readers blocked.
 */
static bool rcu_preempt_has_tasks(struct rcu_node *rnp)
{
	return false;
}

/*
 * Because there is no preemptible RCU, there can be no deferred quiescent
 * states.
 */
static bool rcu_preempt_need_deferred_qs(struct task_struct *t)
{
	return false;
}
static void rcu_preempt_deferred_qs(struct task_struct *t) { }

/*
 * Because there is no preemptible RCU, there can be no readers blocked,
 * so there is no need to check for blocked tasks.  So check only for
 * bogus qsmask values.
 */
static void rcu_preempt_check_blocked_tasks(struct rcu_node *rnp)
{
	WARN_ON_ONCE(rnp->qsmask);
}

/*
 * Check to see if this CPU is in a non-context-switch quiescent state,
 * namely user mode and idle loop.
 */
static void rcu_flavor_sched_clock_irq(int user)
{
	if (user || rcu_is_cpu_rrupt_from_idle()) {

		/*
		 * Get here if this CPU took its interrupt from user
		 * mode or from the idle loop, and if this is not a
		 * nested interrupt.  In this case, the CPU is in
		 * a quiescent state, so note it.
		 *
		 * No memory barrier is required here because rcu_qs()
		 * references only CPU-local variables that other CPUs
		 * neither access nor modify, at least not while the
		 * corresponding CPU is online.
		 */

		rcu_qs();
	}
}

/*
 * Because preemptible RCU does not exist, tasks cannot possibly exit
 * while in preemptible RCU read-side critical sections.
 */
void exit_rcu(void)
{
}

/*
 * Dump the guaranteed-empty blocked-tasks state.  Trust but verify.
 */
static void
dump_blkd_tasks(struct rcu_node *rnp, int ncheck)
{
	WARN_ON_ONCE(!list_empty(&rnp->blkd_tasks));
}

#endif /* #else #ifdef CONFIG_PREEMPT_RCU */

/*
 * If boosting, set rcuc kthreads to realtime priority.
 */
static void rcu_cpu_kthread_setup(unsigned int cpu)
{
#ifdef CONFIG_RCU_BOOST
	struct sched_param sp;

	sp.sched_priority = kthread_prio;
	sched_setscheduler_nocheck(current, SCHED_FIFO, &sp);
#endif /* #ifdef CONFIG_RCU_BOOST */
}

#ifdef CONFIG_RCU_BOOST

/*
 * Carry out RCU priority boosting on the task indicated by ->exp_tasks
 * or ->boost_tasks, advancing the pointer to the next task in the
 * ->blkd_tasks list.
 *
 * Note that irqs must be enabled: boosting the task can block.
 * Returns 1 if there are more tasks needing to be boosted.
 */
static int rcu_boost(struct rcu_node *rnp)
{
	unsigned long flags;
	struct task_struct *t;
	struct list_head *tb;

	if (READ_ONCE(rnp->exp_tasks) == NULL &&
	    READ_ONCE(rnp->boost_tasks) == NULL)
		return 0;  /* Nothing left to boost. */

	raw_spin_lock_irqsave_rcu_node(rnp, flags);

	/*
	 * Recheck under the lock: all tasks in need of boosting
	 * might exit their RCU read-side critical sections on their own.
	 */
	if (rnp->exp_tasks == NULL && rnp->boost_tasks == NULL) {
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		return 0;
	}

	/*
	 * Preferentially boost tasks blocking expedited grace periods.
	 * This cannot starve the normal grace periods because a second
	 * expedited grace period must boost all blocked tasks, including
	 * those blocking the pre-existing normal grace period.
	 */
	if (rnp->exp_tasks != NULL)
		tb = rnp->exp_tasks;
	else
		tb = rnp->boost_tasks;

	/*
	 * We boost task t by manufacturing an rt_mutex that appears to
	 * be held by task t.  We leave a pointer to that rt_mutex where
	 * task t can find it, and task t will release the mutex when it
	 * exits its outermost RCU read-side critical section.  Then
	 * simply acquiring this artificial rt_mutex will boost task
	 * t's priority.  (Thanks to tglx for suggesting this approach!)
	 *
	 * Note that task t must acquire rnp->lock to remove itself from
	 * the ->blkd_tasks list, which it will do from exit() if from
	 * nowhere else.  We therefore are guaranteed that task t will
	 * stay around at least until we drop rnp->lock.  Note that
	 * rnp->lock also resolves races between our priority boosting
	 * and task t's exiting its outermost RCU read-side critical
	 * section.
	 */
	t = container_of(tb, struct task_struct, rcu_node_entry);
	rt_mutex_init_proxy_locked(&rnp->boost_mtx, t);
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	/* Lock only for side effect: boosts task t's priority. */
	rt_mutex_lock(&rnp->boost_mtx);
	rt_mutex_unlock(&rnp->boost_mtx);  /* Then keep lockdep happy. */

	return READ_ONCE(rnp->exp_tasks) != NULL ||
	       READ_ONCE(rnp->boost_tasks) != NULL;
}

/*
 * Priority-boosting kthread, one per leaf rcu_node.
 */
static int rcu_boost_kthread(void *arg)
{
	struct rcu_node *rnp = (struct rcu_node *)arg;
	int spincnt = 0;
	int more2boost;

	trace_rcu_utilization(TPS("Start boost kthread@init"));
	for (;;) {
		rnp->boost_kthread_status = RCU_KTHREAD_WAITING;
		trace_rcu_utilization(TPS("End boost kthread@rcu_wait"));
		rcu_wait(rnp->boost_tasks || rnp->exp_tasks);
		trace_rcu_utilization(TPS("Start boost kthread@rcu_wait"));
		rnp->boost_kthread_status = RCU_KTHREAD_RUNNING;
		more2boost = rcu_boost(rnp);
		if (more2boost)
			spincnt++;
		else
			spincnt = 0;
		if (spincnt > 10) {
			rnp->boost_kthread_status = RCU_KTHREAD_YIELDING;
			trace_rcu_utilization(TPS("End boost kthread@rcu_yield"));
			schedule_timeout_interruptible(2);
			trace_rcu_utilization(TPS("Start boost kthread@rcu_yield"));
			spincnt = 0;
		}
	}
	/* NOTREACHED */
	trace_rcu_utilization(TPS("End boost kthread@notreached"));
	return 0;
}

/*
 * Check to see if it is time to start boosting RCU readers that are
 * blocking the current grace period, and, if so, tell the per-rcu_node
 * kthread to start boosting them.  If there is an expedited grace
 * period in progress, it is always time to boost.
 *
 * The caller must hold rnp->lock, which this function releases.
 * The ->boost_kthread_task is immortal, so we don't need to worry
 * about it going away.
 */
static void rcu_initiate_boost(struct rcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	raw_lockdep_assert_held_rcu_node(rnp);
	if (!rcu_preempt_blocked_readers_cgp(rnp) && rnp->exp_tasks == NULL) {
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		return;
	}
	if (rnp->exp_tasks != NULL ||
	    (rnp->gp_tasks != NULL &&
	     rnp->boost_tasks == NULL &&
	     rnp->qsmask == 0 &&
	     ULONG_CMP_GE(jiffies, rnp->boost_time))) {
		if (rnp->exp_tasks == NULL)
			rnp->boost_tasks = rnp->gp_tasks;
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		rcu_wake_cond(rnp->boost_kthread_task,
			      rnp->boost_kthread_status);
	} else {
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	}
}

/*
 * Is the current CPU running the RCU-callbacks kthread?
 * Caller must have preemption disabled.
 */
static bool rcu_is_callbacks_kthread(void)
{
	return __this_cpu_read(rcu_data.rcu_cpu_kthread_task) == current;
}

#define RCU_BOOST_DELAY_JIFFIES DIV_ROUND_UP(CONFIG_RCU_BOOST_DELAY * HZ, 1000)

/*
 * Do priority-boost accounting for the start of a new grace period.
 */
static void rcu_preempt_boost_start_gp(struct rcu_node *rnp)
{
	rnp->boost_time = jiffies + RCU_BOOST_DELAY_JIFFIES;
}

/*
 * Create an RCU-boost kthread for the specified node if one does not
 * already exist.  We only create this kthread for preemptible RCU.
 * Returns zero if all is well, a negated errno otherwise.
 */
static int rcu_spawn_one_boost_kthread(struct rcu_node *rnp)
{
	int rnp_index = rnp - rcu_get_root();
	unsigned long flags;
	struct sched_param sp;
	struct task_struct *t;

	if (!IS_ENABLED(CONFIG_PREEMPT_RCU))
		return 0;

	if (!rcu_scheduler_fully_active || rcu_rnp_online_cpus(rnp) == 0)
		return 0;

	rcu_state.boost = 1;
	if (rnp->boost_kthread_task != NULL)
		return 0;
	t = kthread_create(rcu_boost_kthread, (void *)rnp,
			   "rcub/%d", rnp_index);
	if (IS_ERR(t))
		return PTR_ERR(t);
	raw_spin_lock_irqsave_rcu_node(rnp, flags);
	rnp->boost_kthread_task = t;
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
	sp.sched_priority = kthread_prio;
	sched_setscheduler_nocheck(t, SCHED_FIFO, &sp);
	wake_up_process(t); /* get to TASK_INTERRUPTIBLE quickly. */
	return 0;
}

/*
 * Set the per-rcu_node kthread's affinity to cover all CPUs that are
 * served by the rcu_node in question.  The CPU hotplug lock is still
 * held, so the value of rnp->qsmaskinit will be stable.
 *
 * We don't include outgoingcpu in the affinity set, use -1 if there is
 * no outgoing CPU.  If there are no CPUs left in the affinity set,
 * this function allows the kthread to execute on any CPU.
 */
static void rcu_boost_kthread_setaffinity(struct rcu_node *rnp, int outgoingcpu)
{
	struct task_struct *t = rnp->boost_kthread_task;
	unsigned long mask = rcu_rnp_online_cpus(rnp);
	cpumask_var_t cm;
	int cpu;

	if (!t)
		return;
	if (!zalloc_cpumask_var(&cm, GFP_KERNEL))
		return;
	for_each_leaf_node_possible_cpu(rnp, cpu)
		if ((mask & leaf_node_cpu_bit(rnp, cpu)) &&
		    cpu != outgoingcpu)
			cpumask_set_cpu(cpu, cm);
	if (cpumask_weight(cm) == 0)
		cpumask_setall(cm);
	set_cpus_allowed_ptr(t, cm);
	free_cpumask_var(cm);
}

/*
 * Spawn boost kthreads -- called as soon as the scheduler is running.
 */
static void __init rcu_spawn_boost_kthreads(void)
{
	struct rcu_node *rnp;

	rcu_for_each_leaf_node(rnp)
		(void)rcu_spawn_one_boost_kthread(rnp);
}

static void rcu_prepare_kthreads(int cpu)
{
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);
	struct rcu_node *rnp = rdp->mynode;

	/* Fire up the incoming CPU's kthread and leaf rcu_node kthread. */
	if (rcu_scheduler_fully_active)
		(void)rcu_spawn_one_boost_kthread(rnp);
}

#else /* #ifdef CONFIG_RCU_BOOST */

static void rcu_initiate_boost(struct rcu_node *rnp, unsigned long flags)
	__releases(rnp->lock)
{
	raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
}

static bool rcu_is_callbacks_kthread(void)
{
	return false;
}

static void rcu_preempt_boost_start_gp(struct rcu_node *rnp)
{
}

static void rcu_boost_kthread_setaffinity(struct rcu_node *rnp, int outgoingcpu)
{
}

static void __init rcu_spawn_boost_kthreads(void)
{
}

static void rcu_prepare_kthreads(int cpu)
{
}

#endif /* #else #ifdef CONFIG_RCU_BOOST */

#if !defined(CONFIG_RCU_FAST_NO_HZ)

/*
 * Check to see if any future RCU-related work will need to be done
 * by the current CPU, even if none need be done immediately, returning
 * 1 if so.  This function is part of the RCU implementation; it is -not-
 * an exported member of the RCU API.
 *
 * Because we not have RCU_FAST_NO_HZ, just check whether or not this
 * CPU has RCU callbacks queued.
 */
int rcu_needs_cpu(u64 basemono, u64 *nextevt)
{
	*nextevt = KTIME_MAX;
	return !rcu_segcblist_empty(&this_cpu_ptr(&rcu_data)->cblist);
}

/*
 * Because we do not have RCU_FAST_NO_HZ, don't bother cleaning up
 * after it.
 */
static void rcu_cleanup_after_idle(void)
{
}

/*
 * Do the idle-entry grace-period work, which, because CONFIG_RCU_FAST_NO_HZ=n,
 * is nothing.
 */
static void rcu_prepare_for_idle(void)
{
}

#else /* #if !defined(CONFIG_RCU_FAST_NO_HZ) */

/*
 * This code is invoked when a CPU goes idle, at which point we want
 * to have the CPU do everything required for RCU so that it can enter
 * the energy-efficient dyntick-idle mode.  This is handled by a
 * state machine implemented by rcu_prepare_for_idle() below.
 *
 * The following three proprocessor symbols control this state machine:
 *
 * RCU_IDLE_GP_DELAY gives the number of jiffies that a CPU is permitted
 *	to sleep in dyntick-idle mode with RCU callbacks pending.  This
 *	is sized to be roughly one RCU grace period.  Those energy-efficiency
 *	benchmarkers who might otherwise be tempted to set this to a large
 *	number, be warned: Setting RCU_IDLE_GP_DELAY too high can hang your
 *	system.  And if you are -that- concerned about energy efficiency,
 *	just power the system down and be done with it!
 * RCU_IDLE_LAZY_GP_DELAY gives the number of jiffies that a CPU is
 *	permitted to sleep in dyntick-idle mode with only lazy RCU
 *	callbacks pending.  Setting this too high can OOM your system.
 *
 * The values below work well in practice.  If future workloads require
 * adjustment, they can be converted into kernel config parameters, though
 * making the state machine smarter might be a better option.
 */
#define RCU_IDLE_GP_DELAY 4		/* Roughly one grace period. */
#define RCU_IDLE_LAZY_GP_DELAY (6 * HZ)	/* Roughly six seconds. */

static int rcu_idle_gp_delay = RCU_IDLE_GP_DELAY;
module_param(rcu_idle_gp_delay, int, 0644);
static int rcu_idle_lazy_gp_delay = RCU_IDLE_LAZY_GP_DELAY;
module_param(rcu_idle_lazy_gp_delay, int, 0644);

/*
 * Try to advance callbacks on the current CPU, but only if it has been
 * awhile since the last time we did so.  Afterwards, if there are any
 * callbacks ready for immediate invocation, return true.
 */
static bool __maybe_unused rcu_try_advance_all_cbs(void)
{
	bool cbs_ready = false;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	struct rcu_node *rnp;

	/* Exit early if we advanced recently. */
	if (jiffies == rdp->last_advance_all)
		return false;
	rdp->last_advance_all = jiffies;

	rnp = rdp->mynode;

	/*
	 * Don't bother checking unless a grace period has
	 * completed since we last checked and there are
	 * callbacks not yet ready to invoke.
	 */
	if ((rcu_seq_completed_gp(rdp->gp_seq,
				  rcu_seq_current(&rnp->gp_seq)) ||
	     unlikely(READ_ONCE(rdp->gpwrap))) &&
	    rcu_segcblist_pend_cbs(&rdp->cblist))
		note_gp_changes(rdp);

	if (rcu_segcblist_ready_cbs(&rdp->cblist))
		cbs_ready = true;
	return cbs_ready;
}

/*
 * Allow the CPU to enter dyntick-idle mode unless it has callbacks ready
 * to invoke.  If the CPU has callbacks, try to advance them.  Tell the
 * caller to set the timeout based on whether or not there are non-lazy
 * callbacks.
 *
 * The caller must have disabled interrupts.
 */
int rcu_needs_cpu(u64 basemono, u64 *nextevt)
{
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	unsigned long dj;

	lockdep_assert_irqs_disabled();

	/* If no callbacks, RCU doesn't need the CPU. */
	if (rcu_segcblist_empty(&rdp->cblist)) {
		*nextevt = KTIME_MAX;
		return 0;
	}

	/* Attempt to advance callbacks. */
	if (rcu_try_advance_all_cbs()) {
		/* Some ready to invoke, so initiate later invocation. */
		invoke_rcu_core();
		return 1;
	}
	rdp->last_accelerate = jiffies;

	/* Request timer delay depending on laziness, and round. */
	rdp->all_lazy = !rcu_segcblist_n_nonlazy_cbs(&rdp->cblist);
	if (rdp->all_lazy) {
		dj = round_jiffies(rcu_idle_lazy_gp_delay + jiffies) - jiffies;
	} else {
		dj = round_up(rcu_idle_gp_delay + jiffies,
			       rcu_idle_gp_delay) - jiffies;
	}
	*nextevt = basemono + dj * TICK_NSEC;
	return 0;
}

/*
 * Prepare a CPU for idle from an RCU perspective.  The first major task
 * is to sense whether nohz mode has been enabled or disabled via sysfs.
 * The second major task is to check to see if a non-lazy callback has
 * arrived at a CPU that previously had only lazy callbacks.  The third
 * major task is to accelerate (that is, assign grace-period numbers to)
 * any recently arrived callbacks.
 *
 * The caller must have disabled interrupts.
 */
static void rcu_prepare_for_idle(void)
{
	bool needwake;
	struct rcu_data *rdp = this_cpu_ptr(&rcu_data);
	struct rcu_node *rnp;
	int tne;

	lockdep_assert_irqs_disabled();
	if (rcu_is_nocb_cpu(smp_processor_id()))
		return;

	/* Handle nohz enablement switches conservatively. */
	tne = READ_ONCE(tick_nohz_active);
	if (tne != rdp->tick_nohz_enabled_snap) {
		if (!rcu_segcblist_empty(&rdp->cblist))
			invoke_rcu_core(); /* force nohz to see update. */
		rdp->tick_nohz_enabled_snap = tne;
		return;
	}
	if (!tne)
		return;

	/*
	 * If a non-lazy callback arrived at a CPU having only lazy
	 * callbacks, invoke RCU core for the side-effect of recalculating
	 * idle duration on re-entry to idle.
	 */
	if (rdp->all_lazy && rcu_segcblist_n_nonlazy_cbs(&rdp->cblist)) {
		rdp->all_lazy = false;
		invoke_rcu_core();
		return;
	}

	/*
	 * If we have not yet accelerated this jiffy, accelerate all
	 * callbacks on this CPU.
	 */
	if (rdp->last_accelerate == jiffies)
		return;
	rdp->last_accelerate = jiffies;
	if (rcu_segcblist_pend_cbs(&rdp->cblist)) {
		rnp = rdp->mynode;
		raw_spin_lock_rcu_node(rnp); /* irqs already disabled. */
		needwake = rcu_accelerate_cbs(rnp, rdp);
		raw_spin_unlock_rcu_node(rnp); /* irqs remain disabled. */
		if (needwake)
			rcu_gp_kthread_wake();
	}
}

/*
 * Clean up for exit from idle.  Attempt to advance callbacks based on
 * any grace periods that elapsed while the CPU was idle, and if any
 * callbacks are now ready to invoke, initiate invocation.
 */
static void rcu_cleanup_after_idle(void)
{
	lockdep_assert_irqs_disabled();
	if (rcu_is_nocb_cpu(smp_processor_id()))
		return;
	if (rcu_try_advance_all_cbs())
		invoke_rcu_core();
}

#endif /* #else #if !defined(CONFIG_RCU_FAST_NO_HZ) */

#ifdef CONFIG_RCU_NOCB_CPU

/*
 * Offload callback processing from the boot-time-specified set of CPUs
 * specified by rcu_nocb_mask.  For the CPUs in the set, there are kthreads
 * created that pull the callbacks from the corresponding CPU, wait for
 * a grace period to elapse, and invoke the callbacks.  These kthreads
 * are organized into leaders, which manage incoming callbacks, wait for
 * grace periods, and awaken followers, and the followers, which only
 * invoke callbacks.  Each leader is its own follower.  The no-CBs CPUs
 * do a wake_up() on their kthread when they insert a callback into any
 * empty list, unless the rcu_nocb_poll boot parameter has been specified,
 * in which case each kthread actively polls its CPU.  (Which isn't so great
 * for energy efficiency, but which does reduce RCU's overhead on that CPU.)
 *
 * This is intended to be used in conjunction with Frederic Weisbecker's
 * adaptive-idle work, which would seriously reduce OS jitter on CPUs
 * running CPU-bound user-mode computations.
 *
 * Offloading of callbacks can also be used as an energy-efficiency
 * measure because CPUs with no RCU callbacks queued are more aggressive
 * about entering dyntick-idle mode.
 */


/*
 * Parse the boot-time rcu_nocb_mask CPU list from the kernel parameters.
 * The string after the "rcu_nocbs=" is either "all" for all CPUs, or a
 * comma-separated list of CPUs and/or CPU ranges.  If an invalid list is
 * given, a warning is emitted and all CPUs are offloaded.
 */
static int __init rcu_nocb_setup(char *str)
{
	alloc_bootmem_cpumask_var(&rcu_nocb_mask);
	if (!strcasecmp(str, "all"))
		cpumask_setall(rcu_nocb_mask);
	else
		if (cpulist_parse(str, rcu_nocb_mask)) {
			pr_warn("rcu_nocbs= bad CPU range, all CPUs set\n");
			cpumask_setall(rcu_nocb_mask);
		}
	return 1;
}
__setup("rcu_nocbs=", rcu_nocb_setup);

static int __init parse_rcu_nocb_poll(char *arg)
{
	rcu_nocb_poll = true;
	return 0;
}
early_param("rcu_nocb_poll", parse_rcu_nocb_poll);

/*
 * Wake up any no-CBs CPUs' kthreads that were waiting on the just-ended
 * grace period.
 */
static void rcu_nocb_gp_cleanup(struct swait_queue_head *sq)
{
	swake_up_all(sq);
}

static struct swait_queue_head *rcu_nocb_gp_get(struct rcu_node *rnp)
{
	return &rnp->nocb_gp_wq[rcu_seq_ctr(rnp->gp_seq) & 0x1];
}

static void rcu_init_one_nocb(struct rcu_node *rnp)
{
	init_swait_queue_head(&rnp->nocb_gp_wq[0]);
	init_swait_queue_head(&rnp->nocb_gp_wq[1]);
}

/* Is the specified CPU a no-CBs CPU? */
bool rcu_is_nocb_cpu(int cpu)
{
	if (cpumask_available(rcu_nocb_mask))
		return cpumask_test_cpu(cpu, rcu_nocb_mask);
	return false;
}

/*
 * Kick the leader kthread for this NOCB group.  Caller holds ->nocb_lock
 * and this function releases it.
 */
static void __wake_nocb_leader(struct rcu_data *rdp, bool force,
			       unsigned long flags)
	__releases(rdp->nocb_lock)
{
	struct rcu_data *rdp_leader = rdp->nocb_leader;

	lockdep_assert_held(&rdp->nocb_lock);
	if (!READ_ONCE(rdp_leader->nocb_kthread)) {
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		return;
	}
	if (rdp_leader->nocb_leader_sleep || force) {
		/* Prior smp_mb__after_atomic() orders against prior enqueue. */
		WRITE_ONCE(rdp_leader->nocb_leader_sleep, false);
		del_timer(&rdp->nocb_timer);
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		smp_mb(); /* ->nocb_leader_sleep before swake_up_one(). */
		swake_up_one(&rdp_leader->nocb_wq);
	} else {
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
	}
}

/*
 * Kick the leader kthread for this NOCB group, but caller has not
 * acquired locks.
 */
static void wake_nocb_leader(struct rcu_data *rdp, bool force)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
	__wake_nocb_leader(rdp, force, flags);
}

/*
 * Arrange to wake the leader kthread for this NOCB group at some
 * future time when it is safe to do so.
 */
static void wake_nocb_leader_defer(struct rcu_data *rdp, int waketype,
				   const char *reason)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
	if (rdp->nocb_defer_wakeup == RCU_NOCB_WAKE_NOT)
		mod_timer(&rdp->nocb_timer, jiffies + 1);
	WRITE_ONCE(rdp->nocb_defer_wakeup, waketype);
	trace_rcu_nocb_wake(rcu_state.name, rdp->cpu, reason);
	raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
}

/* Does rcu_barrier need to queue an RCU callback on the specified CPU?  */
static bool rcu_nocb_cpu_needs_barrier(int cpu)
{
	struct rcu_data *rdp = per_cpu_ptr(&rcu_data, cpu);
	unsigned long ret;
#ifdef CONFIG_PROVE_RCU
	struct rcu_head *rhp;
#endif /* #ifdef CONFIG_PROVE_RCU */

	/*
	 * Check count of all no-CBs callbacks awaiting invocation.
	 * There needs to be a barrier before this function is called,
	 * but associated with a prior determination that no more
	 * callbacks would be posted.  In the worst case, the first
	 * barrier in rcu_barrier() suffices (but the caller cannot
	 * necessarily rely on this, not a substitute for the caller
	 * getting the concurrency design right!).  There must also be a
	 * barrier between the following load and posting of a callback
	 * (if a callback is in fact needed).  This is associated with an
	 * atomic_inc() in the caller.
	 */
	ret = rcu_get_n_cbs_nocb_cpu(rdp);

#ifdef CONFIG_PROVE_RCU
	rhp = READ_ONCE(rdp->nocb_head);
	if (!rhp)
		rhp = READ_ONCE(rdp->nocb_gp_head);
	if (!rhp)
		rhp = READ_ONCE(rdp->nocb_follower_head);

	/* Having no rcuo kthread but CBs after scheduler starts is bad! */
	if (!READ_ONCE(rdp->nocb_kthread) && rhp &&
	    rcu_scheduler_fully_active) {
		/* RCU callback enqueued before CPU first came online??? */
		pr_err("RCU: Never-onlined no-CBs CPU %d has CB %p\n",
		       cpu, rhp->func);
		WARN_ON_ONCE(1);
	}
#endif /* #ifdef CONFIG_PROVE_RCU */

	return !!ret;
}

/*
 * Enqueue the specified string of rcu_head structures onto the specified
 * CPU's no-CBs lists.  The CPU is specified by rdp, the head of the
 * string by rhp, and the tail of the string by rhtp.  The non-lazy/lazy
 * counts are supplied by rhcount and rhcount_lazy.
 *
 * If warranted, also wake up the kthread servicing this CPUs queues.
 */
static void __call_rcu_nocb_enqueue(struct rcu_data *rdp,
				    struct rcu_head *rhp,
				    struct rcu_head **rhtp,
				    int rhcount, int rhcount_lazy,
				    unsigned long flags)
{
	int len;
	struct rcu_head **old_rhpp;
	struct task_struct *t;

	/* Enqueue the callback on the nocb list and update counts. */
	atomic_long_add(rhcount, &rdp->nocb_q_count);
	/* rcu_barrier() relies on ->nocb_q_count add before xchg. */
	old_rhpp = xchg(&rdp->nocb_tail, rhtp);
	WRITE_ONCE(*old_rhpp, rhp);
	atomic_long_add(rhcount_lazy, &rdp->nocb_q_count_lazy);
	smp_mb__after_atomic(); /* Store *old_rhpp before _wake test. */

	/* If we are not being polled and there is a kthread, awaken it ... */
	t = READ_ONCE(rdp->nocb_kthread);
	if (rcu_nocb_poll || !t) {
		trace_rcu_nocb_wake(rcu_state.name, rdp->cpu,
				    TPS("WakeNotPoll"));
		return;
	}
	len = rcu_get_n_cbs_nocb_cpu(rdp);
	if (old_rhpp == &rdp->nocb_head) {
		if (!irqs_disabled_flags(flags)) {
			/* ... if queue was empty ... */
			wake_nocb_leader(rdp, false);
			trace_rcu_nocb_wake(rcu_state.name, rdp->cpu,
					    TPS("WakeEmpty"));
		} else {
			wake_nocb_leader_defer(rdp, RCU_NOCB_WAKE,
					       TPS("WakeEmptyIsDeferred"));
		}
		rdp->qlen_last_fqs_check = 0;
	} else if (len > rdp->qlen_last_fqs_check + qhimark) {
		/* ... or if many callbacks queued. */
		if (!irqs_disabled_flags(flags)) {
			wake_nocb_leader(rdp, true);
			trace_rcu_nocb_wake(rcu_state.name, rdp->cpu,
					    TPS("WakeOvf"));
		} else {
			wake_nocb_leader_defer(rdp, RCU_NOCB_WAKE_FORCE,
					       TPS("WakeOvfIsDeferred"));
		}
		rdp->qlen_last_fqs_check = LONG_MAX / 2;
	} else {
		trace_rcu_nocb_wake(rcu_state.name, rdp->cpu, TPS("WakeNot"));
	}
	return;
}

/*
 * This is a helper for __call_rcu(), which invokes this when the normal
 * callback queue is inoperable.  If this is not a no-CBs CPU, this
 * function returns failure back to __call_rcu(), which can complain
 * appropriately.
 *
 * Otherwise, this function queues the callback where the corresponding
 * "rcuo" kthread can find it.
 */
static bool __call_rcu_nocb(struct rcu_data *rdp, struct rcu_head *rhp,
			    bool lazy, unsigned long flags)
{

	if (!rcu_is_nocb_cpu(rdp->cpu))
		return false;
	__call_rcu_nocb_enqueue(rdp, rhp, &rhp->next, 1, lazy, flags);
	if (__is_kfree_rcu_offset((unsigned long)rhp->func))
		trace_rcu_kfree_callback(rcu_state.name, rhp,
					 (unsigned long)rhp->func,
					 -atomic_long_read(&rdp->nocb_q_count_lazy),
					 -rcu_get_n_cbs_nocb_cpu(rdp));
	else
		trace_rcu_callback(rcu_state.name, rhp,
				   -atomic_long_read(&rdp->nocb_q_count_lazy),
				   -rcu_get_n_cbs_nocb_cpu(rdp));

	/*
	 * If called from an extended quiescent state with interrupts
	 * disabled, invoke the RCU core in order to allow the idle-entry
	 * deferred-wakeup check to function.
	 */
	if (irqs_disabled_flags(flags) &&
	    !rcu_is_watching() &&
	    cpu_online(smp_processor_id()))
		invoke_rcu_core();

	return true;
}

/*
 * Adopt orphaned callbacks on a no-CBs CPU, or return 0 if this is
 * not a no-CBs CPU.
 */
static bool __maybe_unused rcu_nocb_adopt_orphan_cbs(struct rcu_data *my_rdp,
						     struct rcu_data *rdp,
						     unsigned long flags)
{
	lockdep_assert_irqs_disabled();
	if (!rcu_is_nocb_cpu(smp_processor_id()))
		return false; /* Not NOCBs CPU, caller must migrate CBs. */
	__call_rcu_nocb_enqueue(my_rdp, rcu_segcblist_head(&rdp->cblist),
				rcu_segcblist_tail(&rdp->cblist),
				rcu_segcblist_n_cbs(&rdp->cblist),
				rcu_segcblist_n_lazy_cbs(&rdp->cblist), flags);
	rcu_segcblist_init(&rdp->cblist);
	rcu_segcblist_disable(&rdp->cblist);
	return true;
}

/*
 * If necessary, kick off a new grace period, and either way wait
 * for a subsequent grace period to complete.
 */
static void rcu_nocb_wait_gp(struct rcu_data *rdp)
{
	unsigned long c;
	bool d;
	unsigned long flags;
	bool needwake;
	struct rcu_node *rnp = rdp->mynode;

	local_irq_save(flags);
	c = rcu_seq_snap(&rcu_state.gp_seq);
	if (!rdp->gpwrap && ULONG_CMP_GE(rdp->gp_seq_needed, c)) {
		local_irq_restore(flags);
	} else {
		raw_spin_lock_rcu_node(rnp); /* irqs already disabled. */
		needwake = rcu_start_this_gp(rnp, rdp, c);
		raw_spin_unlock_irqrestore_rcu_node(rnp, flags);
		if (needwake)
			rcu_gp_kthread_wake();
	}

	/*
	 * Wait for the grace period.  Do so interruptibly to avoid messing
	 * up the load average.
	 */
	trace_rcu_this_gp(rnp, rdp, c, TPS("StartWait"));
	for (;;) {
		swait_event_interruptible_exclusive(
			rnp->nocb_gp_wq[rcu_seq_ctr(c) & 0x1],
			(d = rcu_seq_done(&rnp->gp_seq, c)));
		if (likely(d))
			break;
		WARN_ON(signal_pending(current));
		trace_rcu_this_gp(rnp, rdp, c, TPS("ResumeWait"));
	}
	trace_rcu_this_gp(rnp, rdp, c, TPS("EndWait"));
	smp_mb(); /* Ensure that CB invocation happens after GP end. */
}

/*
 * Leaders come here to wait for additional callbacks to show up.
 * This function does not return until callbacks appear.
 */
static void nocb_leader_wait(struct rcu_data *my_rdp)
{
	bool firsttime = true;
	unsigned long flags;
	bool gotcbs;
	struct rcu_data *rdp;
	struct rcu_head **tail;

wait_again:

	/* Wait for callbacks to appear. */
	if (!rcu_nocb_poll) {
		trace_rcu_nocb_wake(rcu_state.name, my_rdp->cpu, TPS("Sleep"));
		swait_event_interruptible_exclusive(my_rdp->nocb_wq,
				!READ_ONCE(my_rdp->nocb_leader_sleep));
		raw_spin_lock_irqsave(&my_rdp->nocb_lock, flags);
		my_rdp->nocb_leader_sleep = true;
		WRITE_ONCE(my_rdp->nocb_defer_wakeup, RCU_NOCB_WAKE_NOT);
		del_timer(&my_rdp->nocb_timer);
		raw_spin_unlock_irqrestore(&my_rdp->nocb_lock, flags);
	} else if (firsttime) {
		firsttime = false; /* Don't drown trace log with "Poll"! */
		trace_rcu_nocb_wake(rcu_state.name, my_rdp->cpu, TPS("Poll"));
	}

	/*
	 * Each pass through the following loop checks a follower for CBs.
	 * We are our own first follower.  Any CBs found are moved to
	 * nocb_gp_head, where they await a grace period.
	 */
	gotcbs = false;
	smp_mb(); /* wakeup and _sleep before ->nocb_head reads. */
	for (rdp = my_rdp; rdp; rdp = rdp->nocb_next_follower) {
		rdp->nocb_gp_head = READ_ONCE(rdp->nocb_head);
		if (!rdp->nocb_gp_head)
			continue;  /* No CBs here, try next follower. */

		/* Move callbacks to wait-for-GP list, which is empty. */
		WRITE_ONCE(rdp->nocb_head, NULL);
		rdp->nocb_gp_tail = xchg(&rdp->nocb_tail, &rdp->nocb_head);
		gotcbs = true;
	}

	/* No callbacks?  Sleep a bit if polling, and go retry.  */
	if (unlikely(!gotcbs)) {
		WARN_ON(signal_pending(current));
		if (rcu_nocb_poll) {
			schedule_timeout_interruptible(1);
		} else {
			trace_rcu_nocb_wake(rcu_state.name, my_rdp->cpu,
					    TPS("WokeEmpty"));
		}
		goto wait_again;
	}

	/* Wait for one grace period. */
	rcu_nocb_wait_gp(my_rdp);

	/* Each pass through the following loop wakes a follower, if needed. */
	for (rdp = my_rdp; rdp; rdp = rdp->nocb_next_follower) {
		if (!rcu_nocb_poll &&
		    READ_ONCE(rdp->nocb_head) &&
		    READ_ONCE(my_rdp->nocb_leader_sleep)) {
			raw_spin_lock_irqsave(&my_rdp->nocb_lock, flags);
			my_rdp->nocb_leader_sleep = false;/* No need to sleep.*/
			raw_spin_unlock_irqrestore(&my_rdp->nocb_lock, flags);
		}
		if (!rdp->nocb_gp_head)
			continue; /* No CBs, so no need to wake follower. */

		/* Append callbacks to follower's "done" list. */
		raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
		tail = rdp->nocb_follower_tail;
		rdp->nocb_follower_tail = rdp->nocb_gp_tail;
		*tail = rdp->nocb_gp_head;
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		if (rdp != my_rdp && tail == &rdp->nocb_follower_head) {
			/* List was empty, so wake up the follower.  */
			swake_up_one(&rdp->nocb_wq);
		}
	}

	/* If we (the leader) don't have CBs, go wait some more. */
	if (!my_rdp->nocb_follower_head)
		goto wait_again;
}

/*
 * Followers come here to wait for additional callbacks to show up.
 * This function does not return until callbacks appear.
 */
static void nocb_follower_wait(struct rcu_data *rdp)
{
	for (;;) {
		trace_rcu_nocb_wake(rcu_state.name, rdp->cpu, TPS("FollowerSleep"));
		swait_event_interruptible_exclusive(rdp->nocb_wq,
					 READ_ONCE(rdp->nocb_follower_head));
		if (smp_load_acquire(&rdp->nocb_follower_head)) {
			/* ^^^ Ensure CB invocation follows _head test. */
			return;
		}
		WARN_ON(signal_pending(current));
		trace_rcu_nocb_wake(rcu_state.name, rdp->cpu, TPS("WokeEmpty"));
	}
}

/*
 * Per-rcu_data kthread, but only for no-CBs CPUs.  Each kthread invokes
 * callbacks queued by the corresponding no-CBs CPU, however, there is
 * an optional leader-follower relationship so that the grace-period
 * kthreads don't have to do quite so many wakeups.
 */
static int rcu_nocb_kthread(void *arg)
{
	int c, cl;
	unsigned long flags;
	struct rcu_head *list;
	struct rcu_head *next;
	struct rcu_head **tail;
	struct rcu_data *rdp = arg;

	/* Each pass through this loop invokes one batch of callbacks */
	for (;;) {
		/* Wait for callbacks. */
		if (rdp->nocb_leader == rdp)
			nocb_leader_wait(rdp);
		else
			nocb_follower_wait(rdp);

		/* Pull the ready-to-invoke callbacks onto local list. */
		raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
		list = rdp->nocb_follower_head;
		rdp->nocb_follower_head = NULL;
		tail = rdp->nocb_follower_tail;
		rdp->nocb_follower_tail = &rdp->nocb_follower_head;
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		if (WARN_ON_ONCE(!list))
			continue;
		trace_rcu_nocb_wake(rcu_state.name, rdp->cpu, TPS("WokeNonEmpty"));

		/* Each pass through the following loop invokes a callback. */
		trace_rcu_batch_start(rcu_state.name,
				      atomic_long_read(&rdp->nocb_q_count_lazy),
				      rcu_get_n_cbs_nocb_cpu(rdp), -1);
		c = cl = 0;
		while (list) {
			next = list->next;
			/* Wait for enqueuing to complete, if needed. */
			while (next == NULL && &list->next != tail) {
				trace_rcu_nocb_wake(rcu_state.name, rdp->cpu,
						    TPS("WaitQueue"));
				schedule_timeout_interruptible(1);
				trace_rcu_nocb_wake(rcu_state.name, rdp->cpu,
						    TPS("WokeQueue"));
				next = list->next;
			}
			debug_rcu_head_unqueue(list);
			local_bh_disable();
			if (__rcu_reclaim(rcu_state.name, list))
				cl++;
			c++;
			local_bh_enable();
			cond_resched_tasks_rcu_qs();
			list = next;
		}
		trace_rcu_batch_end(rcu_state.name, c, !!list, 0, 0, 1);
		smp_mb__before_atomic();  /* _add after CB invocation. */
		atomic_long_add(-c, &rdp->nocb_q_count);
		atomic_long_add(-cl, &rdp->nocb_q_count_lazy);
	}
	return 0;
}

/* Is a deferred wakeup of rcu_nocb_kthread() required? */
static int rcu_nocb_need_deferred_wakeup(struct rcu_data *rdp)
{
	return READ_ONCE(rdp->nocb_defer_wakeup);
}

/* Do a deferred wakeup of rcu_nocb_kthread(). */
static void do_nocb_deferred_wakeup_common(struct rcu_data *rdp)
{
	unsigned long flags;
	int ndw;

	raw_spin_lock_irqsave(&rdp->nocb_lock, flags);
	if (!rcu_nocb_need_deferred_wakeup(rdp)) {
		raw_spin_unlock_irqrestore(&rdp->nocb_lock, flags);
		return;
	}
	ndw = READ_ONCE(rdp->nocb_defer_wakeup);
	WRITE_ONCE(rdp->nocb_defer_wakeup, RCU_NOCB_WAKE_NOT);
	__wake_nocb_leader(rdp, ndw == RCU_NOCB_WAKE_FORCE, flags);
	trace_rcu_nocb_wake(rcu_state.name, rdp->cpu, TPS("DeferredWake"));
}

/* Do a deferred wakeup of rcu_nocb_kthread() from a timer handler. */
static void do_nocb_deferred_wakeup_timer(struct timer_list *t)
{
	struct rcu_data *rdp = from_timer(rdp, t, nocb_timer);

	do_nocb_deferred_wakeup_common(rdp);
}

/*
 * Do a deferred wakeup of rcu_nocb_kthread() from fastpath.
 * This means we do an inexact common-case check.  Note that if
 * we miss, ->nocb_timer will eventually clean things up.
 */
static void do_nocb_deferred_wakeup(struct rcu_data *rdp)
{
	if (rcu_nocb_need_deferred_wakeup(rdp))
		do_nocb_deferred_wakeup_common(rdp);
}

void __init rcu_init_nohz(void)
{
	int cpu;
	bool need_rcu_nocb_mask = false;

#if defined(CONFIG_NO_HZ_FULL)
	if (tick_nohz_full_running && cpumask_weight(tick_nohz_full_mask))
		need_rcu_nocb_mask = true;
#endif /* #if defined(CONFIG_NO_HZ_FULL) */

	if (!cpumask_available(rcu_nocb_mask) && need_rcu_nocb_mask) {
		if (!zalloc_cpumask_var(&rcu_nocb_mask, GFP_KERNEL)) {
			pr_info("rcu_nocb_mask allocation failed, callback offloading disabled.\n");
			return;
		}
	}
	if (!cpumask_available(rcu_nocb_mask))
		return;

#if defined(CONFIG_NO_HZ_FULL)
	if (tick_nohz_full_running)
		cpumask_or(rcu_nocb_mask, rcu_nocb_mask, tick_nohz_full_mask);
#endif /* #if defined(CONFIG_NO_HZ_FULL) */

	if (!cpumask_subset(rcu_nocb_mask, cpu_possible_mask)) {
		pr_info("\tNote: kernel parameter 'rcu_nocbs=', 'nohz_full', or 'isolcpus=' contains nonexistent CPUs.\n");
		cpumask_and(rcu_nocb_mask, cpu_possible_mask,
			    rcu_nocb_mask);
	}
	if (cpumask_empty(rcu_nocb_mask))
		pr_info("\tOffload RCU callbacks from CPUs: (none).\n");
	else
		pr_info("\tOffload RCU callbacks from CPUs: %*pbl.\n",
			cpumask_pr_args(rcu_nocb_mask));
	if (rcu_nocb_poll)
		pr_info("\tPoll for callbacks from no-CBs CPUs.\n");

	for_each_cpu(cpu, rcu_nocb_mask)
		init_nocb_callback_list(per_cpu_ptr(&rcu_data, cpu));
	rcu_organize_nocb_kthreads();
}

/* Initialize per-rcu_data variables for no-CBs CPUs. */
static void __init rcu_boot_init_nocb_percpu_data(struct rcu_data *rdp)
{
	rdp->nocb_tail = &rdp->nocb_head;
	init_swait_queue_head(&rdp->nocb_wq);
	rdp->nocb_follower_tail = &rdp->nocb_follower_head;
	raw_spin_lock_init(&rdp->nocb_lock);
	timer_setup(&rdp->nocb_timer, do_nocb_deferred_wakeup_timer, 0);
}

/*
 * If the specified CPU is a no-CBs CPU that does not already have its
 * rcuo kthread, spawn it.  If the CPUs are brought online out of order,
 * this can require re-organizing the leader-follower relationships.
 */
static void rcu_spawn_one_nocb_kthread(int cpu)
{
	struct rcu_data *rdp;
	struct rcu_data *rdp_last;
	struct rcu_data *rdp_old_leader;
	struct rcu_data *rdp_spawn = per_cpu_ptr(&rcu_data, cpu);
	struct task_struct *t;

	/*
	 * If this isn't a no-CBs CPU or if it already has an rcuo kthread,
	 * then nothing to do.
	 */
	if (!rcu_is_nocb_cpu(cpu) || rdp_spawn->nocb_kthread)
		return;

	/* If we didn't spawn the leader first, reorganize! */
	rdp_old_leader = rdp_spawn->nocb_leader;
	if (rdp_old_leader != rdp_spawn && !rdp_old_leader->nocb_kthread) {
		rdp_last = NULL;
		rdp = rdp_old_leader;
		do {
			rdp->nocb_leader = rdp_spawn;
			if (rdp_last && rdp != rdp_spawn)
				rdp_last->nocb_next_follower = rdp;
			if (rdp == rdp_spawn) {
				rdp = rdp->nocb_next_follower;
			} else {
				rdp_last = rdp;
				rdp = rdp->nocb_next_follower;
				rdp_last->nocb_next_follower = NULL;
			}
		} while (rdp);
		rdp_spawn->nocb_next_follower = rdp_old_leader;
	}

	/* Spawn the kthread for this CPU. */
	t = kthread_run(rcu_nocb_kthread, rdp_spawn,
			"rcuo%c/%d", rcu_state.abbr, cpu);
	if (WARN_ONCE(IS_ERR(t), "%s: Could not start rcuo kthread, OOM is now expected behavior\n", __func__))
		return;
	WRITE_ONCE(rdp_spawn->nocb_kthread, t);
}

/*
 * If the specified CPU is a no-CBs CPU that does not already have its
 * rcuo kthread, spawn it.
 */
static void rcu_spawn_cpu_nocb_kthread(int cpu)
{
	if (rcu_scheduler_fully_active)
		rcu_spawn_one_nocb_kthread(cpu);
}

/*
 * Once the scheduler is running, spawn rcuo kthreads for all online
 * no-CBs CPUs.  This assumes that the early_initcall()s happen before
 * non-boot CPUs come online -- if this changes, we will need to add
 * some mutual exclusion.
 */
static void __init rcu_spawn_nocb_kthreads(void)
{
	int cpu;

	for_each_online_cpu(cpu)
		rcu_spawn_cpu_nocb_kthread(cpu);
}

/* How many follower CPU IDs per leader?  Default of -1 for sqrt(nr_cpu_ids). */
static int rcu_nocb_leader_stride = -1;
module_param(rcu_nocb_leader_stride, int, 0444);

/*
 * Initialize leader-follower relationships for all no-CBs CPU.
 */
static void __init rcu_organize_nocb_kthreads(void)
{
	int cpu;
	int ls = rcu_nocb_leader_stride;
	int nl = 0;  /* Next leader. */
	struct rcu_data *rdp;
	struct rcu_data *rdp_leader = NULL;  /* Suppress misguided gcc warn. */
	struct rcu_data *rdp_prev = NULL;

	if (!cpumask_available(rcu_nocb_mask))
		return;
	if (ls == -1) {
		ls = int_sqrt(nr_cpu_ids);
		rcu_nocb_leader_stride = ls;
	}

	/*
	 * Each pass through this loop sets up one rcu_data structure.
	 * Should the corresponding CPU come online in the future, then
	 * we will spawn the needed set of rcu_nocb_kthread() kthreads.
	 */
	for_each_cpu(cpu, rcu_nocb_mask) {
		rdp = per_cpu_ptr(&rcu_data, cpu);
		if (rdp->cpu >= nl) {
			/* New leader, set up for followers & next leader. */
			nl = DIV_ROUND_UP(rdp->cpu + 1, ls) * ls;
			rdp->nocb_leader = rdp;
			rdp_leader = rdp;
		} else {
			/* Another follower, link to previous leader. */
			rdp->nocb_leader = rdp_leader;
			rdp_prev->nocb_next_follower = rdp;
		}
		rdp_prev = rdp;
	}
}

/* Prevent __call_rcu() from enqueuing callbacks on no-CBs CPUs */
static bool init_nocb_callback_list(struct rcu_data *rdp)
{
	if (!rcu_is_nocb_cpu(rdp->cpu))
		return false;

	/* If there are early-boot callbacks, move them to nocb lists. */
	if (!rcu_segcblist_empty(&rdp->cblist)) {
		rdp->nocb_head = rcu_segcblist_head(&rdp->cblist);
		rdp->nocb_tail = rcu_segcblist_tail(&rdp->cblist);
		atomic_long_set(&rdp->nocb_q_count,
				rcu_segcblist_n_cbs(&rdp->cblist));
		atomic_long_set(&rdp->nocb_q_count_lazy,
				rcu_segcblist_n_lazy_cbs(&rdp->cblist));
		rcu_segcblist_init(&rdp->cblist);
	}
	rcu_segcblist_disable(&rdp->cblist);
	return true;
}

/*
 * Bind the current task to the offloaded CPUs.  If there are no offloaded
 * CPUs, leave the task unbound.  Splat if the bind attempt fails.
 */
void rcu_bind_current_to_nocb(void)
{
	if (cpumask_available(rcu_nocb_mask) && cpumask_weight(rcu_nocb_mask))
		WARN_ON(sched_setaffinity(current->pid, rcu_nocb_mask));
}
EXPORT_SYMBOL_GPL(rcu_bind_current_to_nocb);

/*
 * Return the number of RCU callbacks still queued from the specified
 * CPU, which must be a nocbs CPU.
 */
static unsigned long rcu_get_n_cbs_nocb_cpu(struct rcu_data *rdp)
{
	return atomic_long_read(&rdp->nocb_q_count);
}

#else /* #ifdef CONFIG_RCU_NOCB_CPU */

static bool rcu_nocb_cpu_needs_barrier(int cpu)
{
	WARN_ON_ONCE(1); /* Should be dead code. */
	return false;
}

static void rcu_nocb_gp_cleanup(struct swait_queue_head *sq)
{
}

static struct swait_queue_head *rcu_nocb_gp_get(struct rcu_node *rnp)
{
	return NULL;
}

static void rcu_init_one_nocb(struct rcu_node *rnp)
{
}

static bool __call_rcu_nocb(struct rcu_data *rdp, struct rcu_head *rhp,
			    bool lazy, unsigned long flags)
{
	return false;
}

static bool __maybe_unused rcu_nocb_adopt_orphan_cbs(struct rcu_data *my_rdp,
						     struct rcu_data *rdp,
						     unsigned long flags)
{
	return false;
}

static void __init rcu_boot_init_nocb_percpu_data(struct rcu_data *rdp)
{
}

static int rcu_nocb_need_deferred_wakeup(struct rcu_data *rdp)
{
	return false;
}

static void do_nocb_deferred_wakeup(struct rcu_data *rdp)
{
}

static void rcu_spawn_cpu_nocb_kthread(int cpu)
{
}

static void __init rcu_spawn_nocb_kthreads(void)
{
}

static bool init_nocb_callback_list(struct rcu_data *rdp)
{
	return false;
}

static unsigned long rcu_get_n_cbs_nocb_cpu(struct rcu_data *rdp)
{
	return 0;
}

#endif /* #else #ifdef CONFIG_RCU_NOCB_CPU */

/*
 * Is this CPU a NO_HZ_FULL CPU that should ignore RCU so that the
 * grace-period kthread will do force_quiescent_state() processing?
 * The idea is to avoid waking up RCU core processing on such a
 * CPU unless the grace period has extended for too long.
 *
 * This code relies on the fact that all NO_HZ_FULL CPUs are also
 * CONFIG_RCU_NOCB_CPU CPUs.
 */
static bool rcu_nohz_full_cpu(void)
{
#ifdef CONFIG_NO_HZ_FULL
	if (tick_nohz_full_cpu(smp_processor_id()) &&
	    (!rcu_gp_in_progress() ||
	     ULONG_CMP_LT(jiffies, READ_ONCE(rcu_state.gp_start) + HZ)))
		return true;
#endif /* #ifdef CONFIG_NO_HZ_FULL */
	return false;
}

/*
 * Bind the RCU grace-period kthreads to the housekeeping CPU.
 */
static void rcu_bind_gp_kthread(void)
{
	if (!tick_nohz_full_enabled())
		return;
	housekeeping_affine(current, HK_FLAG_RCU);
}

/* Record the current task on dyntick-idle entry. */
static void rcu_dynticks_task_enter(void)
{
#if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL)
	WRITE_ONCE(current->rcu_tasks_idle_cpu, smp_processor_id());
#endif /* #if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL) */
}

/* Record no current task on dyntick-idle exit. */
static void rcu_dynticks_task_exit(void)
{
#if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL)
	WRITE_ONCE(current->rcu_tasks_idle_cpu, -1);
#endif /* #if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL) */
}
