// SPDX-License-Identifier: GPL-2.0+
/*
 * RCU segmented callback lists, function definitions
 *
 * Copyright IBM Corporation, 2017
 *
 * Authors: Paul E. McKenney <paulmck@linux.ibm.com>
 */

#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "rcu_segcblist.h"

/* Initialize simple callback list. */
void rcu_cblist_init(struct rcu_cblist *rclp)
{
	rclp->head = NULL;
	rclp->tail = &rclp->head;
	rclp->len = 0;
}

/*
 * Enqueue an rcu_head structure onto the specified callback list.
 */
void rcu_cblist_enqueue(struct rcu_cblist *rclp, struct rcu_head *rhp)
{
	*rclp->tail = rhp;
	rclp->tail = &rhp->next;
	WRITE_ONCE(rclp->len, rclp->len + 1);
}

/*
 * Flush the second rcu_cblist structure onto the first one, obliterating
 * any contents of the first.  If rhp is non-NULL, enqueue it as the sole
 * element of the second rcu_cblist structure, but ensuring that the second
 * rcu_cblist structure, if initially non-empty, always appears non-empty
 * throughout the process.  If rdp is NULL, the second rcu_cblist structure
 * is instead initialized to empty.
 */
void rcu_cblist_flush_enqueue(struct rcu_cblist *drclp,
			      struct rcu_cblist *srclp,
			      struct rcu_head *rhp)
{
	drclp->head = srclp->head;
	if (drclp->head)
		drclp->tail = srclp->tail;
	else
		drclp->tail = &drclp->head;
	drclp->len = srclp->len;
	if (!rhp) {
		rcu_cblist_init(srclp);
	} else {
		rhp->next = NULL;
		srclp->head = rhp;
		srclp->tail = &rhp->next;
		WRITE_ONCE(srclp->len, 1);
	}
}

/*
 * Dequeue the oldest rcu_head structure from the specified callback
 * list.
 */
struct rcu_head *rcu_cblist_dequeue(struct rcu_cblist *rclp)
{
	struct rcu_head *rhp;

	rhp = rclp->head;
	if (!rhp)
		return NULL;
	rclp->len--;
	rclp->head = rhp->next;
	if (!rclp->head)
		rclp->tail = &rclp->head;
	return rhp;
}

/* Set the length of an rcu_segcblist structure. */
/*
 * IAMROOT, 2023.07.22:
 * - @rsclp->len = v;
 */
static void rcu_segcblist_set_len(struct rcu_segcblist *rsclp, long v)
{
#ifdef CONFIG_RCU_NOCB_CPU
	atomic_long_set(&rsclp->len, v);
#else
	WRITE_ONCE(rsclp->len, v);
#endif
}

/* Get the length of a segment of the rcu_segcblist structure. */
static long rcu_segcblist_get_seglen(struct rcu_segcblist *rsclp, int seg)
{
	return READ_ONCE(rsclp->seglen[seg]);
}

/* Return number of callbacks in segmented callback list by summing seglen. */
long rcu_segcblist_n_segment_cbs(struct rcu_segcblist *rsclp)
{
	long len = 0;
	int i;

	for (i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS; i++)
		len += rcu_segcblist_get_seglen(rsclp, i);

	return len;
}

/* Set the length of a segment of the rcu_segcblist structure. */
/*
 * IAMROOT, 2023.07.22:
 * - seglen[seg] = v;
 */
static void rcu_segcblist_set_seglen(struct rcu_segcblist *rsclp, int seg, long v)
{
	WRITE_ONCE(rsclp->seglen[seg], v);
}

/* Increase the numeric length of a segment by a specified amount. */
/*
 * IAMROOT, 2023.07.22:
 * - @seg에 해당하는 seglen을 +v 한다.
 */
static void rcu_segcblist_add_seglen(struct rcu_segcblist *rsclp, int seg, long v)
{
	WRITE_ONCE(rsclp->seglen[seg], rsclp->seglen[seg] + v);
}

/* Move from's segment length to to's segment. */
/*
 * IAMROOT, 2023.07.22:
 * - @from의 len을 @to으로 옮긴다.
 */
static void rcu_segcblist_move_seglen(struct rcu_segcblist *rsclp, int from, int to)
{
	long len;

	if (from == to)
		return;

	len = rcu_segcblist_get_seglen(rsclp, from);
	if (!len)
		return;

	rcu_segcblist_add_seglen(rsclp, to, len);
	rcu_segcblist_set_seglen(rsclp, from, 0);
}

/* Increment segment's length. */
/*
 * IAMROOT, 2023.07.22:
 * - @seg에 해당하는 seglen을 1증가시킨다.
 */
static void rcu_segcblist_inc_seglen(struct rcu_segcblist *rsclp, int seg)
{
	rcu_segcblist_add_seglen(rsclp, seg, 1);
}

/*
 * Increase the numeric length of an rcu_segcblist structure by the
 * specified amount, which can be negative.  This can cause the ->len
 * field to disagree with the actual number of callbacks on the structure.
 * This increase is fully ordered with respect to the callers accesses
 * both before and after.
 *
 * So why on earth is a memory barrier required both before and after
 * the update to the ->len field???
 *
 * The reason is that rcu_barrier() locklessly samples each CPU's ->len
 * field, and if a given CPU's field is zero, avoids IPIing that CPU.
 * This can of course race with both queuing and invoking of callbacks.
 * Failing to correctly handle either of these races could result in
 * rcu_barrier() failing to IPI a CPU that actually had callbacks queued
 * which rcu_barrier() was obligated to wait on.  And if rcu_barrier()
 * failed to wait on such a callback, unloading certain kernel modules
 * would result in calls to functions whose code was no longer present in
 * the kernel, for but one example.
 *
 * Therefore, ->len transitions from 1->0 and 0->1 have to be carefully
 * ordered with respect with both list modifications and the rcu_barrier().
 *
 * The queuing case is CASE 1 and the invoking case is CASE 2.
 *
 * CASE 1: Suppose that CPU 0 has no callbacks queued, but invokes
 * call_rcu() just as CPU 1 invokes rcu_barrier().  CPU 0's ->len field
 * will transition from 0->1, which is one of the transitions that must
 * be handled carefully.  Without the full memory barriers after the ->len
 * update and at the beginning of rcu_barrier(), the following could happen:
 *
 * CPU 0				CPU 1
 *
 * call_rcu().
 *					rcu_barrier() sees ->len as 0.
 * set ->len = 1.
 *					rcu_barrier() does nothing.
 *					module is unloaded.
 * callback invokes unloaded function!
 *
 * With the full barriers, any case where rcu_barrier() sees ->len as 0 will
 * have unambiguously preceded the return from the racing call_rcu(), which
 * means that this call_rcu() invocation is OK to not wait on.  After all,
 * you are supposed to make sure that any problematic call_rcu() invocations
 * happen before the rcu_barrier().
 *
 *
 * CASE 2: Suppose that CPU 0 is invoking its last callback just as
 * CPU 1 invokes rcu_barrier().  CPU 0's ->len field will transition from
 * 1->0, which is one of the transitions that must be handled carefully.
 * Without the full memory barriers before the ->len update and at the
 * end of rcu_barrier(), the following could happen:
 *
 * CPU 0				CPU 1
 *
 * start invoking last callback
 * set ->len = 0 (reordered)
 *					rcu_barrier() sees ->len as 0
 *					rcu_barrier() does nothing.
 *					module is unloaded
 * callback executing after unloaded!
 *
 * With the full barriers, any case where rcu_barrier() sees ->len as 0
 * will be fully ordered after the completion of the callback function,
 * so that the module unloading operation is completely safe.
 *
 */
/*
 * IAMROOT, 2023.07.22:
 * - papago
 *   rcu_segcblist 구조의 숫자 길이를 지정된 양(음수일 수 있음)만큼 늘립니다. 
 *   이로 인해 ->len 필드가 구조의 실제 콜백 수와 일치하지 않을 수 있습니다.
 *   이 증가는 전후 호출자 액세스와 관련하여 완전히 정렬됩니다.
 *
 *   그렇다면 도대체 왜 ->len 필드에 대한 업데이트 전후에 메모리 배리어가 
 *   필요한 걸까요??? 그 이유는 rcu_barrier()가 각 CPU의 ->len 필드를 잠그지 
 *   않고 샘플링하고 주어진 CPU의 필드가 0이면 해당 CPU의 IPI를 방지하기 
 *   때문입니다.
 *   이것은 물론 대기 및 콜백 호출과 경쟁할 수 있습니다.
 *   이러한 경쟁 중 하나를 올바르게 처리하지 못하면 rcu_barrier()가 대기해야 
 *   하는 콜백이 실제로 대기열에 있는 CPU를 IPI하는 데 실패하는 rcu_barrier()가 
 *   발생할 수 있습니다. 그리고 rcu_barrier()가 그러한 콜백을 기다리는 데 
 *   실패하면 특정 커널 모듈을 언로드하면 코드가 커널에 더 이상 존재하지 
 *   않는 함수가 호출됩니다.
 *
 *   따라서 1->0 및 0->1에서 ->len 전환은 목록 수정 및 rcu_barrier() 모두와 
 *   관련하여 신중하게 정렬해야 합니다. 
 *
 *   큐잉 케이스는 CASE 1이고 호출 케이스는 CASE 2입니다.
 *
 *   사례 1:
 *   CPU 0에는 대기 중인 콜백이 없지만 CPU 1이 rcu_barrier()를 호출하는 
 *   것처럼 call_rcu()를 호출한다고 가정합니다. CPU 0의 ->len 필드는 
 *   0->1에서 전환되며 이는 신중하게 처리해야 하는 전환 중 하나입니다. 
 *   ->len 업데이트 후 및 rcu_barrier() 시작 시 전체 메모리 배리어가 없으면 
 *   다음과 같은 일이 발생할 수 있습니다.
 *
 *   CPU 0				CPU 1
 *
 *   call_rcu().
 *  					rcu_barrier() sees ->len as 0.
 *   set ->len = 1.
 *  					rcu_barrier() does nothing.
 *  					module is unloaded.
 *   
 *
 */
void rcu_segcblist_add_len(struct rcu_segcblist *rsclp, long v)
{
#ifdef CONFIG_RCU_NOCB_CPU
	smp_mb__before_atomic(); // Read header comment above.
	atomic_long_add(v, &rsclp->len);
	smp_mb__after_atomic();  // Read header comment above.
#else
	smp_mb(); // Read header comment above.
	WRITE_ONCE(rsclp->len, rsclp->len + v);
	smp_mb(); // Read header comment above.
#endif
}

/*
 * Increase the numeric length of an rcu_segcblist structure by one.
 * This can cause the ->len field to disagree with the actual number of
 * callbacks on the structure.  This increase is fully ordered with respect
 * to the callers accesses both before and after.
 */
/*
 * IAMROOT, 2023.07.22:
 * - papago
 *  rcu_segcblist 구조체의 숫자 길이를 1 늘립니다.
 *  이로 인해 ->len 필드가 구조의 실제 콜백 수와 일치하지 않을 수 있습니다. 
 *  이 증가는 전후 호출자 액세스와 관련하여 완전히 정렬됩니다.
 *
 * - @rsclp len을 1 증가시킨다.
 */
void rcu_segcblist_inc_len(struct rcu_segcblist *rsclp)
{
	rcu_segcblist_add_len(rsclp, 1);
}

/*
 * Initialize an rcu_segcblist structure.
 */
/*
 * IAMROOT, 2023.07.22:
 * - 원형리스트초기화,
 *   seglen의 모든값과 len을 0으로 초기화하고 SEGCBLIST_ENABLED을 set한다.
 */
void rcu_segcblist_init(struct rcu_segcblist *rsclp)
{
	int i;

	BUILD_BUG_ON(RCU_NEXT_TAIL + 1 != ARRAY_SIZE(rsclp->gp_seq));
	BUILD_BUG_ON(ARRAY_SIZE(rsclp->tails) != ARRAY_SIZE(rsclp->gp_seq));
	rsclp->head = NULL;
	for (i = 0; i < RCU_CBLIST_NSEGS; i++) {
		rsclp->tails[i] = &rsclp->head;
		rcu_segcblist_set_seglen(rsclp, i, 0);
	}
	rcu_segcblist_set_len(rsclp, 0);
	rcu_segcblist_set_flags(rsclp, SEGCBLIST_ENABLED);
}

/*
 * Disable the specified rcu_segcblist structure, so that callbacks can
 * no longer be posted to it.  This structure must be empty.
 */
void rcu_segcblist_disable(struct rcu_segcblist *rsclp)
{
	WARN_ON_ONCE(!rcu_segcblist_empty(rsclp));
	WARN_ON_ONCE(rcu_segcblist_n_cbs(rsclp));
	rcu_segcblist_clear_flags(rsclp, SEGCBLIST_ENABLED);
}

/*
 * Mark the specified rcu_segcblist structure as offloaded.
 */
void rcu_segcblist_offload(struct rcu_segcblist *rsclp, bool offload)
{
	if (offload) {
		rcu_segcblist_clear_flags(rsclp, SEGCBLIST_SOFTIRQ_ONLY);
		rcu_segcblist_set_flags(rsclp, SEGCBLIST_OFFLOADED);
	} else {
		rcu_segcblist_clear_flags(rsclp, SEGCBLIST_OFFLOADED);
	}
}

/*
 * Does the specified rcu_segcblist structure contain callbacks that
 * are ready to be invoked?
 */
bool rcu_segcblist_ready_cbs(struct rcu_segcblist *rsclp)
{
	return rcu_segcblist_is_enabled(rsclp) &&
	       &rsclp->head != READ_ONCE(rsclp->tails[RCU_DONE_TAIL]);
}

/*
 * Does the specified rcu_segcblist structure contain callbacks that
 * are still pending, that is, not yet ready to be invoked?
 */
/*
 * IAMROOT, 2023.07.22:
 * - DONE구간 이후의 대기중인 cb이 있으면 true.
 */
bool rcu_segcblist_pend_cbs(struct rcu_segcblist *rsclp)
{
	return rcu_segcblist_is_enabled(rsclp) &&
	       !rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL);
}

/*
 * Return a pointer to the first callback in the specified rcu_segcblist
 * structure.  This is useful for diagnostics.
 */
struct rcu_head *rcu_segcblist_first_cb(struct rcu_segcblist *rsclp)
{
	if (rcu_segcblist_is_enabled(rsclp))
		return rsclp->head;
	return NULL;
}

/*
 * Return a pointer to the first pending callback in the specified
 * rcu_segcblist structure.  This is useful just after posting a given
 * callback -- if that callback is the first pending callback, then
 * you cannot rely on someone else having already started up the required
 * grace period.
 */
struct rcu_head *rcu_segcblist_first_pend_cb(struct rcu_segcblist *rsclp)
{
	if (rcu_segcblist_is_enabled(rsclp))
		return *rsclp->tails[RCU_DONE_TAIL];
	return NULL;
}

/*
 * Return false if there are no CBs awaiting grace periods, otherwise,
 * return true and store the nearest waited-upon grace period into *lp.
 */
bool rcu_segcblist_nextgp(struct rcu_segcblist *rsclp, unsigned long *lp)
{
	if (!rcu_segcblist_pend_cbs(rsclp))
		return false;
	*lp = rsclp->gp_seq[RCU_WAIT_TAIL];
	return true;
}

/*
 * Enqueue the specified callback onto the specified rcu_segcblist
 * structure, updating accounting as needed.  Note that the ->len
 * field may be accessed locklessly, hence the WRITE_ONCE().
 * The ->len field is used by rcu_barrier() and friends to determine
 * if it must post a callback on this structure, and it is OK
 * for rcu_barrier() to sometimes post callbacks needlessly, but
 * absolutely not OK for it to ever miss posting a callback.
 */
/*
 * IAMROOT, 2023.07.22:
 * - papago
 *   지정된 rcu_segcblist 구조에 지정된 콜백을 큐에 넣고 필요에 
 *   따라 계정을 업데이트합니다. ->len 필드는 잠기지 않고 액세스할 
 *   수 있으므로 WRITE_ONCE()입니다.
 *   ->len 필드는 rcu_barrier()와 그 친구들이 이 구조에 콜백을 
 *   게시해야 하는지 여부를 결정하는 데 사용되며, rcu_barrier()가 
 *   때때로 불필요하게 콜백을 게시하는 것은 괜찮지만 콜백 게시를 
 *   놓치는 것은 절대 좋지 않습니다.
 *
 * - @rsclp의 마지막에 @rhp를 추가한다.
 */
void rcu_segcblist_enqueue(struct rcu_segcblist *rsclp,
			   struct rcu_head *rhp)
{
/*
 * IAMROOT, 2023.07.22:
 * - len++
 */
	rcu_segcblist_inc_len(rsclp);
/*
 * IAMROOT, 2023.07.22:
 * - RCU_NEXT_TAIL len++
 */
	rcu_segcblist_inc_seglen(rsclp, RCU_NEXT_TAIL);
/*
 * IAMROOT, 2023.07.22:
 * - @rhp가 end이므로 next를 NULL로 한다.
 */
	rhp->next = NULL;
/*
 * IAMROOT, 2023.07.22:
 * - end인 @rhp을 마지막 node로 설정한다.
 */
	WRITE_ONCE(*rsclp->tails[RCU_NEXT_TAIL], rhp);
/*
 * IAMROOT, 2023.07.22:
 * - end였던 node의 next끝에 rhp를 연결한다.
 */
	WRITE_ONCE(rsclp->tails[RCU_NEXT_TAIL], &rhp->next);
}

/*
 * Entrain the specified callback onto the specified rcu_segcblist at
 * the end of the last non-empty segment.  If the entire rcu_segcblist
 * is empty, make no change, but return false.
 *
 * This is intended for use by rcu_barrier()-like primitives, -not-
 * for normal grace-period use.  IMPORTANT:  The callback you enqueue
 * will wait for all prior callbacks, NOT necessarily for a grace
 * period.  You have been warned.
 */
bool rcu_segcblist_entrain(struct rcu_segcblist *rsclp,
			   struct rcu_head *rhp)
{
	int i;

	if (rcu_segcblist_n_cbs(rsclp) == 0)
		return false;
	rcu_segcblist_inc_len(rsclp);
	smp_mb(); /* Ensure counts are updated before callback is entrained. */
	rhp->next = NULL;
	for (i = RCU_NEXT_TAIL; i > RCU_DONE_TAIL; i--)
		if (rsclp->tails[i] != rsclp->tails[i - 1])
			break;
	rcu_segcblist_inc_seglen(rsclp, i);
	WRITE_ONCE(*rsclp->tails[i], rhp);
	for (; i <= RCU_NEXT_TAIL; i++)
		WRITE_ONCE(rsclp->tails[i], &rhp->next);
	return true;
}

/*
 * Extract only those callbacks ready to be invoked from the specified
 * rcu_segcblist structure and place them in the specified rcu_cblist
 * structure.
 */
void rcu_segcblist_extract_done_cbs(struct rcu_segcblist *rsclp,
				    struct rcu_cblist *rclp)
{
	int i;

	if (!rcu_segcblist_ready_cbs(rsclp))
		return; /* Nothing to do. */
	rclp->len = rcu_segcblist_get_seglen(rsclp, RCU_DONE_TAIL);
	*rclp->tail = rsclp->head;
	WRITE_ONCE(rsclp->head, *rsclp->tails[RCU_DONE_TAIL]);
	WRITE_ONCE(*rsclp->tails[RCU_DONE_TAIL], NULL);
	rclp->tail = rsclp->tails[RCU_DONE_TAIL];
	for (i = RCU_CBLIST_NSEGS - 1; i >= RCU_DONE_TAIL; i--)
		if (rsclp->tails[i] == rsclp->tails[RCU_DONE_TAIL])
			WRITE_ONCE(rsclp->tails[i], &rsclp->head);
	rcu_segcblist_set_seglen(rsclp, RCU_DONE_TAIL, 0);
}

/*
 * Extract only those callbacks still pending (not yet ready to be
 * invoked) from the specified rcu_segcblist structure and place them in
 * the specified rcu_cblist structure.  Note that this loses information
 * about any callbacks that might have been partway done waiting for
 * their grace period.  Too bad!  They will have to start over.
 */
void rcu_segcblist_extract_pend_cbs(struct rcu_segcblist *rsclp,
				    struct rcu_cblist *rclp)
{
	int i;

	if (!rcu_segcblist_pend_cbs(rsclp))
		return; /* Nothing to do. */
	rclp->len = 0;
	*rclp->tail = *rsclp->tails[RCU_DONE_TAIL];
	rclp->tail = rsclp->tails[RCU_NEXT_TAIL];
	WRITE_ONCE(*rsclp->tails[RCU_DONE_TAIL], NULL);
	for (i = RCU_DONE_TAIL + 1; i < RCU_CBLIST_NSEGS; i++) {
		rclp->len += rcu_segcblist_get_seglen(rsclp, i);
		WRITE_ONCE(rsclp->tails[i], rsclp->tails[RCU_DONE_TAIL]);
		rcu_segcblist_set_seglen(rsclp, i, 0);
	}
}

/*
 * Insert counts from the specified rcu_cblist structure in the
 * specified rcu_segcblist structure.
 */
void rcu_segcblist_insert_count(struct rcu_segcblist *rsclp,
				struct rcu_cblist *rclp)
{
	rcu_segcblist_add_len(rsclp, rclp->len);
}

/*
 * Move callbacks from the specified rcu_cblist to the beginning of the
 * done-callbacks segment of the specified rcu_segcblist.
 */
void rcu_segcblist_insert_done_cbs(struct rcu_segcblist *rsclp,
				   struct rcu_cblist *rclp)
{
	int i;

	if (!rclp->head)
		return; /* No callbacks to move. */
	rcu_segcblist_add_seglen(rsclp, RCU_DONE_TAIL, rclp->len);
	*rclp->tail = rsclp->head;
	WRITE_ONCE(rsclp->head, rclp->head);
	for (i = RCU_DONE_TAIL; i < RCU_CBLIST_NSEGS; i++)
		if (&rsclp->head == rsclp->tails[i])
			WRITE_ONCE(rsclp->tails[i], rclp->tail);
		else
			break;
	rclp->head = NULL;
	rclp->tail = &rclp->head;
}

/*
 * Move callbacks from the specified rcu_cblist to the end of the
 * new-callbacks segment of the specified rcu_segcblist.
 */
void rcu_segcblist_insert_pend_cbs(struct rcu_segcblist *rsclp,
				   struct rcu_cblist *rclp)
{
	if (!rclp->head)
		return; /* Nothing to do. */

	rcu_segcblist_add_seglen(rsclp, RCU_NEXT_TAIL, rclp->len);
	WRITE_ONCE(*rsclp->tails[RCU_NEXT_TAIL], rclp->head);
	WRITE_ONCE(rsclp->tails[RCU_NEXT_TAIL], rclp->tail);
}

/*
 * Advance the callbacks in the specified rcu_segcblist structure based
 * on the current value passed in for the grace-period counter.
 */
/*
 * IAMROOT, 2023.07.22:
 * - papago
 *   gp 카운터에 대해 전달된 현재 값을 기반으로 지정된 rcu_segcblist 
 *   구조에서 콜백을 진행합니다.
 *
 * - complete된 seq이하구간들중 wait, ready구간만 done으로 옮긴다.
 *   next_ready구간은 seq에 관계없이 wait로 옮긴다.
 */
void rcu_segcblist_advance(struct rcu_segcblist *rsclp, unsigned long seq)
{
	int i, j;

	WARN_ON_ONCE(!rcu_segcblist_is_enabled(rsclp));
/*
 * IAMROOT, 2023.07.22:
 * - 완료 구간 이후가 비어있으면 return.
 */
	if (rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL))
		return;

	/*
	 * Find all callbacks whose ->gp_seq numbers indicate that they
	 * are ready to invoke, and put them into the RCU_DONE_TAIL segment.
	 */
/*
 * IAMROOT, 2023.07.22:
 * - papgo
 *   ->gp_seq 번호가 호출 준비가 되었음을 나타내는 모든 콜백을 찾아 
 *   RCU_DONE_TAIL 세그먼트에 넣습니다.
 * - wait 구간 부터 마지막 전까지 done으로 옮기고, 개수도 옮긴다.
 */
	for (i = RCU_WAIT_TAIL; i < RCU_NEXT_TAIL; i++) {
/*
 * IAMROOT, 2023.07.22:
 * - 이미 완료 됬다면 break.
 * - ex) @seq를 20라고 가정. 
 *   
 *     DONE WAIT NEXT_READY NEXT      DONE WAIT NEXT_READY NEXT
 *      12  16       20      24  =>    12                   24
 *                                     16
 *                                     20
 *
 *   20이하를 DONE으로 이동
 *
 * - ex) @seq를 20라고 가정. 20보다 높은 NEXT_READY는 WAIT,
 *     20는 DONE으로 이동
 *   
 *     DONE WAIT NEXT_READY NEXT        DONE WAIT NEXT_READY NEXT
 *      16  20       24      28   =>     16         24        28
 *                                       20
 *   20이하를 DONE으로 이동
 *
 *   list개념적으론 위와 비슷해지지만 20미만은 이제 없어질것이다.
 */
		if (ULONG_CMP_LT(seq, rsclp->gp_seq[i]))
			break;
		WRITE_ONCE(rsclp->tails[RCU_DONE_TAIL], rsclp->tails[i]);
		rcu_segcblist_move_seglen(rsclp, i, RCU_DONE_TAIL);
	}

	/* If no callbacks moved, nothing more need be done. */
/*
 * IAMROOT, 2023.07.22:
 * - wait부터 break가 발생했으면 할게없다. return.
 */
	if (i == RCU_WAIT_TAIL)
		return;
/*
 * IAMROOT, 2023.07.22:
 * - ptr을 옮기기 시작한다.
 */
	/* Clean up tail pointers that might have been misordered above. */
/*
 * IAMROOT, 2023.07.22:
 * - 옮긴 seg를 done tail로 가리킨다. 즉 empty로 만드는것
 * - WAIT까지 처리한경우 i = 2(NEXT_READY)
 *   WAIT -> DONE
 *   
 * - NEXT_READY까지 처리 i = 3(NEXT)
 *   WAIT -> DONE
 *   NEXT_READY -> DONE
 */
	for (j = RCU_WAIT_TAIL; j < i; j++)
		WRITE_ONCE(rsclp->tails[j], rsclp->tails[RCU_DONE_TAIL]);

	/*
	 * Callbacks moved, so clean up the misordered ->tails[] pointers
	 * that now point into the middle of the list of ready-to-invoke
	 * callbacks.  The overall effect is to copy down the later pointers
	 * into the gap that was created by the now-ready segments.
	 */
/*
 * IAMROOT, 2023.07.22:
 * - papago
 *   콜백이 이동되었으므로 이제 호출할 준비가 된 콜백 목록의 중간을 
 *   가리키는 잘못 정렬된 ->tails[] 포인터를 정리하십시오. 전반적인 효과는 
 *   나중 포인터를 지금 준비된 세그먼트에 의해 생성된 간격으로 복사하는 
 *   것입니다.
 *
 * - i = 2 로 끝났을때 NEXT_READY를 WAIT자리로 이동시키기 위한 코드.
 *   for문자체가 큰 의미는 없다.
 */
	for (j = RCU_WAIT_TAIL; i < RCU_NEXT_TAIL; i++, j++) {
/*
 * IAMROOT, 2023.07.22:
 * - next로 향해있으면 대상이 아니다.
 *   ex) WAIT까지 처리한경우 i = 2(NEXT_READY) 에서
 *   WAIT -> DONE의 처리를 위해서 했지만
 *   NEXT_READY는 건드리지 않았다.
 *   만약 NEXT_READY -> NEXT를 가리키고 있다면 여기서 break.
 *   그게 아니라면 WAIT로 옮겨갈것이다.
 */
		if (rsclp->tails[j] == rsclp->tails[RCU_NEXT_TAIL])
			break;  /* No more callbacks. */
/*
 * IAMROOT, 2023.07.22:
 * - DONE <- WAIT, WAIT <- NEXT_READY로 옮긴다. len도 같이 옮긴다.
 */
		WRITE_ONCE(rsclp->tails[j], rsclp->tails[i]);
		rcu_segcblist_move_seglen(rsclp, i, j);
		rsclp->gp_seq[j] = rsclp->gp_seq[i];
	}
}

/*
 * "Accelerate" callbacks based on more-accurate grace-period information.
 * The reason for this is that RCU does not synchronize the beginnings and
 * ends of grace periods, and that callbacks are posted locally.  This in
 * turn means that the callbacks must be labelled conservatively early
 * on, as getting exact information would degrade both performance and
 * scalability.  When more accurate grace-period information becomes
 * available, previously posted callbacks can be "accelerated", marking
 * them to complete at the end of the earlier grace period.
 *
 * This function operates on an rcu_segcblist structure, and also the
 * grace-period sequence number seq at which new callbacks would become
 * ready to invoke.  Returns true if there are callbacks that won't be
 * ready to invoke until seq, false otherwise.
 */
/*
 * IAMROOT, 2023.07.22:
 * - papago
 * - ING
 */
bool rcu_segcblist_accelerate(struct rcu_segcblist *rsclp, unsigned long seq)
{
	int i, j;

	WARN_ON_ONCE(!rcu_segcblist_is_enabled(rsclp));
	if (rcu_segcblist_restempty(rsclp, RCU_DONE_TAIL))
		return false;

	/*
	 * Find the segment preceding the oldest segment of callbacks
	 * whose ->gp_seq[] completion is at or after that passed in via
	 * "seq", skipping any empty segments.  This oldest segment, along
	 * with any later segments, can be merged in with any newly arrived
	 * callbacks in the RCU_NEXT_TAIL segment, and assigned "seq"
	 * as their ->gp_seq[] grace-period completion sequence number.
	 */
	for (i = RCU_NEXT_READY_TAIL; i > RCU_DONE_TAIL; i--)
		if (rsclp->tails[i] != rsclp->tails[i - 1] &&
		    ULONG_CMP_LT(rsclp->gp_seq[i], seq))
			break;

	/*
	 * If all the segments contain callbacks that correspond to
	 * earlier grace-period sequence numbers than "seq", leave.
	 * Assuming that the rcu_segcblist structure has enough
	 * segments in its arrays, this can only happen if some of
	 * the non-done segments contain callbacks that really are
	 * ready to invoke.  This situation will get straightened
	 * out by the next call to rcu_segcblist_advance().
	 *
	 * Also advance to the oldest segment of callbacks whose
	 * ->gp_seq[] completion is at or after that passed in via "seq",
	 * skipping any empty segments.
	 *
	 * Note that segment "i" (and any lower-numbered segments
	 * containing older callbacks) will be unaffected, and their
	 * grace-period numbers remain unchanged.  For example, if i ==
	 * WAIT_TAIL, then neither WAIT_TAIL nor DONE_TAIL will be touched.
	 * Instead, the CBs in NEXT_TAIL will be merged with those in
	 * NEXT_READY_TAIL and the grace-period number of NEXT_READY_TAIL
	 * would be updated.  NEXT_TAIL would then be empty.
	 */
	if (rcu_segcblist_restempty(rsclp, i) || ++i >= RCU_NEXT_TAIL)
		return false;

	/* Accounting: everything below i is about to get merged into i. */
	for (j = i + 1; j <= RCU_NEXT_TAIL; j++)
		rcu_segcblist_move_seglen(rsclp, j, i);

	/*
	 * Merge all later callbacks, including newly arrived callbacks,
	 * into the segment located by the for-loop above.  Assign "seq"
	 * as the ->gp_seq[] value in order to correctly handle the case
	 * where there were no pending callbacks in the rcu_segcblist
	 * structure other than in the RCU_NEXT_TAIL segment.
	 */
	for (; i < RCU_NEXT_TAIL; i++) {
		WRITE_ONCE(rsclp->tails[i], rsclp->tails[RCU_NEXT_TAIL]);
		rsclp->gp_seq[i] = seq;
	}
	return true;
}

/*
 * Merge the source rcu_segcblist structure into the destination
 * rcu_segcblist structure, then initialize the source.  Any pending
 * callbacks from the source get to start over.  It is best to
 * advance and accelerate both the destination and the source
 * before merging.
 */
void rcu_segcblist_merge(struct rcu_segcblist *dst_rsclp,
			 struct rcu_segcblist *src_rsclp)
{
	struct rcu_cblist donecbs;
	struct rcu_cblist pendcbs;

	lockdep_assert_cpus_held();

	rcu_cblist_init(&donecbs);
	rcu_cblist_init(&pendcbs);

	rcu_segcblist_extract_done_cbs(src_rsclp, &donecbs);
	rcu_segcblist_extract_pend_cbs(src_rsclp, &pendcbs);

	/*
	 * No need smp_mb() before setting length to 0, because CPU hotplug
	 * lock excludes rcu_barrier.
	 */
	rcu_segcblist_set_len(src_rsclp, 0);

	rcu_segcblist_insert_count(dst_rsclp, &donecbs);
	rcu_segcblist_insert_count(dst_rsclp, &pendcbs);
	rcu_segcblist_insert_done_cbs(dst_rsclp, &donecbs);
	rcu_segcblist_insert_pend_cbs(dst_rsclp, &pendcbs);

	rcu_segcblist_init(src_rsclp);
}
