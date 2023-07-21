======================================================
A Tour Through TREE_RCU's Grace-Period Memory Ordering
======================================================

August 8, 2017

This article was contributed by Paul E. McKenney

Introduction
============

This document gives a rough visual overview of how Tree RCU's
grace-period memory ordering guarantee is provided.

이 문서는 Tree RCU의 유예 기간 메모리 순서 보장이 제공되는 
방법에 대한 대략적인 시각적 개요를 제공합니다.

What Is Tree RCU's Grace Period Memory Ordering Guarantee?
==========================================================

RCU grace periods provide extremely strong memory-ordering guarantees
for non-idle non-offline code.
Any code that happens after the end of a given RCU grace period is guaranteed
to see the effects of all accesses prior to the beginning of that grace
period that are within RCU read-side critical sections.
Similarly, any code that happens before the beginning of a given RCU grace
period is guaranteed to not see the effects of all accesses following the end
of that grace period that are within RCU read-side critical sections.

Note well that RCU-sched read-side critical sections include any region
of code for which preemption is disabled.
Given that each individual machine instruction can be thought of as
an extremely small region of preemption-disabled code, one can think of
``synchronize_rcu()`` as ``smp_mb()`` on steroids.

RCU updaters use this guarantee by splitting their updates into
two phases, one of which is executed before the grace period and
the other of which is executed after the grace period.
In the most common use case, phase one removes an element from
a linked RCU-protected data structure, and phase two frees that element.
For this to work, any readers that have witnessed state prior to the
phase-one update (in the common case, removal) must not witness state
following the phase-two update (in the common case, freeing).

The RCU implementation provides this guarantee using a network
of lock-based critical sections, memory barriers, and per-CPU
processing, as is described in the following sections.

RCU 유예 기간은 유휴 상태가 아닌 비오프라인 코드에 대해 매우 강력한 
메모리 순서 지정을 보장합니다.
지정된 RCU 유예 기간이 끝난 후 발생하는 모든 코드는 RCU 읽기 측 임계 섹션 
내에 있는 해당 유예 기간이 시작되기 전에 모든 액세스의 영향을 확인하도록 
보장됩니다.
마찬가지로 주어진 RCU 유예 기간이 시작되기 전에 발생하는 모든 코드는 RCU 읽기 
측 임계 섹션 내에 있는 해당 유예 기간이 끝난 후 모든 액세스의 영향을 보지 
않도록 보장됩니다.

RCU가 예약한 읽기 측 중요 섹션에는 선점이 비활성화된 코드 영역이 포함됩니다.
각각의 개별 기계 명령이 선점 비활성화 코드의 극히 작은 영역으로 생각할 수 
있다는 점을 감안할 때 synchronize_rcu()를 스테로이드의 smp_mb()로 생각할 수 
있습니다.

RCU 업데이터는 업데이트를 두 단계로 나누어 이 보장을 사용합니다. 그 중 하나는 
유예 기간 이전에 실행되고 다른 하나는 유예 기간 이후에 실행됩니다.
가장 일반적인 사용 사례에서 1단계는 연결된 RCU 보호 데이터 struct에서 요소를 
제거하고 2단계는 해당 요소를 해제합니다.
이것이 작동하려면 1단계 업데이트(일반적인 경우 제거) 이전의 상태를 목격한 
독자는 2단계 업데이트(일반적인 경우 해제) 이후의 상태를 목격해서는 안 됩니다.

RCU 구현은 다음 섹션에 설명된 대로 잠금 기반 임계 섹션, 메모리 장벽 및 
CPU당 처리의 네트워크를 사용하여 이러한 보장을 제공합니다.

Tree RCU Grace Period Memory Ordering Building Blocks
=====================================================

The workhorse for RCU's grace-period memory ordering is the
critical section for the ``rcu_node`` structure's
``->lock``. These critical sections use helper functions for lock
acquisition, including ``raw_spin_lock_rcu_node()``,
``raw_spin_lock_irq_rcu_node()``, and ``raw_spin_lock_irqsave_rcu_node()``.
Their lock-release counterparts are ``raw_spin_unlock_rcu_node()``,
``raw_spin_unlock_irq_rcu_node()``, and
``raw_spin_unlock_irqrestore_rcu_node()``, respectively.
For completeness, a ``raw_spin_trylock_rcu_node()`` is also provided.
The key point is that the lock-acquisition functions, including
``raw_spin_trylock_rcu_node()``, all invoke ``smp_mb__after_unlock_lock()``
immediately after successful acquisition of the lock.

Therefore, for any given ``rcu_node`` structure, any access
happening before one of the above lock-release functions will be seen
by all CPUs as happening before any access happening after a later
one of the above lock-acquisition functions.
Furthermore, any access happening before one of the
above lock-release function on any given CPU will be seen by all
CPUs as happening before any access happening after a later one
of the above lock-acquisition functions executing on that same CPU,
even if the lock-release and lock-acquisition functions are operating
on different ``rcu_node`` structures.
Tree RCU uses these two ordering guarantees to form an ordering
network among all CPUs that were in any way involved in the grace
period, including any CPUs that came online or went offline during
the grace period in question.

The following litmus test exhibits the ordering effects of these
lock-acquisition and lock-release functions::

RCU의 유예 기간 메모리 순서 지정을 위한 작업 도구는 ``rcu_node`` struct의 
``->lock`` 에 대한 중요한 섹션입니다. 이러한 중요한 섹션은 잠금 획득을 
위해 ``raw_spin_lock_rcu_node()``, ``raw_spin_lock_irq_rcu_node()`` 및 
``raw_spin_lock_irqsave_rcu_node()`` 를 포함하여 헬퍼 함수를 사용합니다.
잠금 해제 상대는 각각 ``raw_spin_unlock_rcu_node()``, 
``raw_spin_unlock_irq_rcu_node()`` 및 
``raw_spin_unlock_irqrestore_rcu_node()`` 입니다.
완전성을 위해 ``raw_spin_trylock_rcu_node()`` 도 제공됩니다.
요점은 ``raw_spin_trylock_rcu_node()`` 를 포함한 잠금 획득 함수가 모두 
성공적으로 잠금을 획득한 직후에 ``smp_mb__after_unlock_lock()`` 을 
호출한다는 것입니다.

따라서 임의의 주어진 ``rcu_node`` struct에 대해 위의 잠금 해제 기능 중 
하나 이전에 발생하는 모든 액세스는 위의 잠금 획득 기능 중 이후에 발생하는 
액세스 이전에 발생하는 것으로 모든 CPU에서 볼 수 있습니다.
또한 주어진 CPU에서 위의 잠금 해제 기능 중 하나 전에 발생하는 모든 
액세스는 잠금 해제 및 잠금 획득 기능이 다른 ``rcu_node`` struct에서 
작동하는 경우에도 동일한 CPU에서 실행되는 위의 잠금 획득 기능 중 
이후에 발생하는 액세스 전에 발생하는 것으로 모든 CPU에서 볼 수 있습니다.
트리 RCU는 이 두 가지 순서 보장을 사용하여 문제의 유예 기간 동안 온라인 
상태가 되거나 오프라인 상태가 된 CPU를 포함하여 유예 기간에 어떤 
방식으로든 관련된 모든 CPU 간에 순서 지정 네트워크를 형성합니다.

다음 리트머스 테스트는 이러한 잠금 획득 및 잠금 해제 기능의 순서 
지정 효과를 나타냅니다::

    1 int x, y, z;
    2
    3 void task0(void)
    4 {
    5   raw_spin_lock_rcu_node(rnp);
    6   WRITE_ONCE(x, 1);
    7   r1 = READ_ONCE(y);
    8   raw_spin_unlock_rcu_node(rnp);
    9 }
   10
   11 void task1(void)
   12 {
   13   raw_spin_lock_rcu_node(rnp);
   14   WRITE_ONCE(y, 1);
   15   r2 = READ_ONCE(z);
   16   raw_spin_unlock_rcu_node(rnp);
   17 }
   18
   19 void task2(void)
   20 {
   21   WRITE_ONCE(z, 1);
   22   smp_mb();
   23   r3 = READ_ONCE(x);
   24 }
   25
   26 WARN_ON(r1 == 0 && r2 == 0 && r3 == 0);

The ``WARN_ON()`` is evaluated at "the end of time",
after all changes have propagated throughout the system.
Without the ``smp_mb__after_unlock_lock()`` provided by the
acquisition functions, this ``WARN_ON()`` could trigger, for example
on PowerPC.
The ``smp_mb__after_unlock_lock()`` invocations prevent this
``WARN_ON()`` from triggering.

``WARN_ON()`` 은 모든 변경 사항이 시스템 전체에 전파된 후 시간이 끝날 
때 평가됩니다.
획득 함수에서 제공하는 ``smp_mb__after_unlock_lock()`` 이 없으면 예를 들어 
PowerPC에서 이 ``WARN_ON()`` 이 트리거될 수 있습니다.
``smp_mb__after_unlock_lock()`` 호출은 이 ``WARN_ON()`` 이 트리거되는 
것을 방지합니다.

+-----------------------------------------------------------------------+
| **Quick Quiz**:                                                       |
+-----------------------------------------------------------------------+
| But the chain of rcu_node-structure lock acquisitions guarantees      |
| that new readers will see all of the updater's pre-grace-period       |
| accesses and also guarantees that the updater's post-grace-period     |
| accesses will see all of the old reader's accesses.  So why do we     |
| need all of those calls to smp_mb__after_unlock_lock()?               |
|                                                                       |
| 그러나 rcu_node struct 잠금 획득 체인은 새 독자가 업데이터의 유예     |
| 기간 이전 액세스를 모두 볼 수 있도록 보장하고 업데이터의 유예 기간    |
| 이후 액세스가 이전 독자의 모든 액세스를 볼 수 있도록 보장합니다.      |
| 그렇다면 smp_mb__after_unlock_lock()에 대한 모든 호출이 필요한 이유는 |
| 무엇입니까?                                                           |
+-----------------------------------------------------------------------+
| **Answer**:                                                           |
+-----------------------------------------------------------------------+
| Because we must provide ordering for RCU's polling grace-period       |
| primitives, for example, get_state_synchronize_rcu() and              |
| poll_state_synchronize_rcu().  Consider this code::                   |
|                                                                       |
|  CPU 0                                     CPU 1                      |
|  ----                                      ----                       |
|  WRITE_ONCE(X, 1)                          WRITE_ONCE(Y, 1)           |
|  g = get_state_synchronize_rcu()           smp_mb()                   |
|  while (!poll_state_synchronize_rcu(g))    r1 = READ_ONCE(X)          |
|          continue;                                                    |
|  r0 = READ_ONCE(Y)                                                    |
|                                                                       |
| RCU guarantees that the outcome r0 == 0 && r1 == 0 will not           |
| happen, even if CPU 1 is in an RCU extended quiescent state           |
| (idle or offline) and thus won't interact directly with the RCU       |
| core processing at all.                                               |
|                                                                       |
| RCU의 폴링 유예 기간 프리미티브(예: get_state_synchronize_rcu() 및    |
| poll_state_synchronize_rcu())에 대한 순서를 제공해야 하기 때문입니다. |
| 다음 코드를 고려하십시오.                                             |
|                                                                       |
|  CPU 0                                     CPU 1                      |
|  ----                                      ----                       |
|  WRITE_ONCE(X, 1)                          WRITE_ONCE(Y, 1)           |
|  g = get_state_synchronize_rcu()           smp_mb()                   |
|  while (!poll_state_synchronize_rcu(g))    r1 = READ_ONCE(X)          |
|          continue;                                                    |
|  r0 = READ_ONCE(Y)                                                    |
|                                                                       |
| RCU는 CPU 1이 RCU 확장 대기 상태(유휴 또는 오프라인)에 있고 따라서    |
| RCU 코어 처리와 전혀 직접 상호 작용하지 않더라도 결과                 |
| r0 == 0 && r1 == 0이 발생하지 않도록 보장합니다.                      |
+-----------------------------------------------------------------------+

This approach must be extended to include idle CPUs, which need
RCU's grace-period memory ordering guarantee to extend to any
RCU read-side critical sections preceding and following the current
idle sojourn.
This case is handled by calls to the strongly ordered
``atomic_add_return()`` read-modify-write atomic operation that
is invoked within ``rcu_dynticks_eqs_enter()`` at idle-entry
time and within ``rcu_dynticks_eqs_exit()`` at idle-exit time.
The grace-period kthread invokes ``rcu_dynticks_snap()`` and
``rcu_dynticks_in_eqs_since()`` (both of which invoke
an ``atomic_add_return()`` of zero) to detect idle CPUs.

이 접근 방식은 유휴 CPU를 포함하도록 확장되어야 하며, 현재 유휴 체류 
전후의 모든 RCU 읽기 측 임계 섹션으로 확장하기 위해 RCU의 유예 기간 메모리 
순서 보장이 필요합니다.
이 경우는 ``rcu_dynticks_eqs_enter()`` 내에서 유휴 진입 시간에 호출되고 
``rcu_dynticks_eqs_exit()`` 내에서 유휴 종료 시간에 호출되는 강력하게 
정렬된 ``atomic_add_return()`` 읽기-수정-쓰기 원자 연산에 대한 호출로 
처리됩니다.
유예 기간 kthread는 유휴 CPU를 감지하기 위해 ``rcu_dynticks_snap()`` 및 
``rcu_dynticks_in_eqs_since()`` (둘 다 0의 ``atomic_add_return()`` 를 호출함)를 
호출합니다.

+-----------------------------------------------------------------------+
| **Quick Quiz**:                                                       |
+-----------------------------------------------------------------------+
| But what about CPUs that remain offline for the entire grace period?  |
|                                                                       |
| 그러나 전체 유예 기간 동안 오프라인 상태를 유지하는 CPU는 어떻게      |
| 됩니까?                                                               |
+-----------------------------------------------------------------------+
| **Answer**:                                                           |
+-----------------------------------------------------------------------+
| Such CPUs will be offline at the beginning of the grace period, so    |
| the grace period won't expect quiescent states from them. Races       |
| between grace-period start and CPU-hotplug operations are mediated    |
| by the CPU's leaf ``rcu_node`` structure's ``->lock`` as described    |
| above.                                                                |
|                                                                       |
| 이러한 CPU는 유예 기간이 시작될 때 오프라인 상태가 되므로 유예 기간은 |
| 정지 상태를 기대하지 않습니다. 유예 기간 시작과 CPU 핫플러그 작업     |
| 간의 경합은 위에서 설명한 대로 CPU의 리프 ``rcu_node`` struct의       |
| ``->lock`` 에 의해 조정됩니다.                                        |
+-----------------------------------------------------------------------+

The approach must be extended to handle one final case, that of waking a
task blocked in ``synchronize_rcu()``. This task might be affinitied to
a CPU that is not yet aware that the grace period has ended, and thus
might not yet be subject to the grace period's memory ordering.
Therefore, there is an ``smp_mb()`` after the return from
``wait_for_completion()`` in the ``synchronize_rcu()`` code path.

마지막 경우인 ``synchronize_rcu()`` 에서 차단된 작업을 깨우는 경우를 
처리하도록 접근 방식을 확장해야 합니다. 이 작업은 유예 기간이 
종료되었음을 아직 인식하지 못하는 CPU와 연관될 수 있으므로 아직 유예 
기간의 메모리 순서 지정이 적용되지 않을 수 있습니다.
따라서 ``synchronize_rcu()`` 코드 경로에서 ``wait_for_completion()`` 
에서 반환된 후 ``smp_mb()`` 가 있습니다.

+-----------------------------------------------------------------------+
| **Quick Quiz**:                                                       |
+-----------------------------------------------------------------------+
| What? Where??? I don't see any ``smp_mb()`` after the return from     |
| ``wait_for_completion()``!!!                                          |
|                                                                       |
| 무엇? 어디??? ``wait_for_completion()`` 에서 반환된 후 ``smp_mb()``   |
| 가 표시되지 않습니다!!!                                               | 
+-----------------------------------------------------------------------+
+-----------------------------------------------------------------------+
| **Answer**:                                                           |
+-----------------------------------------------------------------------+
| That would be because I spotted the need for that ``smp_mb()`` during |
| the creation of this documentation, and it is therefore unlikely to   |
| hit mainline before v4.14. Kudos to Lance Roy, Will Deacon, Peter     |
| Zijlstra, and Jonathan Cameron for asking questions that sensitized   |
| me to the rather elaborate sequence of events that demonstrate the    |
| need for this memory barrier.                                         |
|                                                                       |
| 이 문서를 작성하는 동안 smp_mb()의 필요성을 발견했기 때문에 v4.14     |
| 이전에는 메인라인에 도달할 가능성이 낮습니다. Lance Roy, Will Deacon, |
| Peter Zijlstra, Jonathan Cameron에게 이 메모리 장벽의 필요성을        |
| 보여주는 다소 정교한 일련의 사건에 대해 저를 민감하게 만드는 질문을   |
| 해주신 것에 찬사를 보냅니다.                                          |
+-----------------------------------------------------------------------+

Tree RCU's grace--period memory-ordering guarantees rely most heavily on
the ``rcu_node`` structure's ``->lock`` field, so much so that it is
necessary to abbreviate this pattern in the diagrams in the next
section. For example, consider the ``rcu_prepare_for_idle()`` function
shown below, which is one of several functions that enforce ordering of
newly arrived RCU callbacks against future grace periods:

트리 RCU의 유예 기간 메모리 순서 지정 보장은 ``rcu_node`` struct의 
``->lock`` 필드에 가장 많이 의존하므로 다음 섹션의 다이어그램에서 
이 패턴을 축약할 필요가 있습니다. 예를 들어, 아래에 표시된 
``rcu_prepare_for_idle()`` 함수를 고려하십시오. 이 함수는 향후 유예 
기간에 대해 새로 도착한 RCU 콜백의 순서를 강제하는 여러 함수 중 하나입니다.

::

    1 static void rcu_prepare_for_idle(void)
    2 {
    3   bool needwake;
    4   struct rcu_data *rdp;
    5   struct rcu_dynticks *rdtp = this_cpu_ptr(&rcu_dynticks);
    6   struct rcu_node *rnp;
    7   struct rcu_state *rsp;
    8   int tne;
    9
   10   if (IS_ENABLED(CONFIG_RCU_NOCB_CPU_ALL) ||
   11       rcu_is_nocb_cpu(smp_processor_id()))
   12     return;
   13   tne = READ_ONCE(tick_nohz_active);
   14   if (tne != rdtp->tick_nohz_enabled_snap) {
   15     if (rcu_cpu_has_callbacks(NULL))
   16       invoke_rcu_core();
   17     rdtp->tick_nohz_enabled_snap = tne;
   18     return;
   19   }
   20   if (!tne)
   21     return;
   22   if (rdtp->all_lazy &&
   23       rdtp->nonlazy_posted != rdtp->nonlazy_posted_snap) {
   24     rdtp->all_lazy = false;
   25     rdtp->nonlazy_posted_snap = rdtp->nonlazy_posted;
   26     invoke_rcu_core();
   27     return;
   28   }
   29   if (rdtp->last_accelerate == jiffies)
   30     return;
   31   rdtp->last_accelerate = jiffies;
   32   for_each_rcu_flavor(rsp) {
   33     rdp = this_cpu_ptr(rsp->rda);
   34     if (rcu_segcblist_pend_cbs(&rdp->cblist))
   35       continue;
   36     rnp = rdp->mynode;
   37     raw_spin_lock_rcu_node(rnp);
   38     needwake = rcu_accelerate_cbs(rsp, rnp, rdp);
   39     raw_spin_unlock_rcu_node(rnp);
   40     if (needwake)
   41       rcu_gp_kthread_wake(rsp);
   42   }
   43 }

But the only part of ``rcu_prepare_for_idle()`` that really matters for
this discussion are lines 37–39. We will therefore abbreviate this
function as follows:

그러나 ``rcu_prepare_for_idle()`` 에서 이 논의에서 정말로 중요한 유일한 
부분은 37–39행입니다. 따라서 이 기능을 다음과 같이 축약합니다.

.. kernel-figure:: rcu_node-lock.svg

The box represents the ``rcu_node`` structure's ``->lock`` critical
section, with the double line on top representing the additional

상자는 ``rcu_node`` struct의 ``->lock`` 임계 섹션을 나타내며 상단의 
이중선은 추가를 나타냅니다.

``smp_mb__after_unlock_lock()``.

Tree RCU Grace Period Memory Ordering Components
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Tree RCU's grace-period memory-ordering guarantee is provided by a
number of RCU components:

트리 RCU의 유예 기간 메모리 순서 보장은 여러 RCU 구성 요소에서 
제공됩니다.

#. `Callback Registry`_
#. `Grace-Period Initialization`_
#. `Self-Reported Quiescent States`_
#. `Dynamic Tick Interface`_
#. `CPU-Hotplug Interface`_
#. `Forcing Quiescent States`_
#. `Grace-Period Cleanup`_
#. `Callback Invocation`_

Each of the following section looks at the corresponding component in
detail.

다음 각 섹션에서는 해당 구성 요소를 자세히 살펴봅니다.

Callback Registry
^^^^^^^^^^^^^^^^^

If RCU's grace-period guarantee is to mean anything at all, any access
that happens before a given invocation of ``call_rcu()`` must also
happen before the corresponding grace period. The implementation of this
portion of RCU's grace period guarantee is shown in the following
figure:

RCU의 유예 기간 보장이 의미하는 바가 있다면 주어진 ``call_rcu()`` 호출 
이전에 발생하는 모든 액세스는 해당 유예 기간 이전에도 발생해야 합니다. 
RCU의 유예 기간 보장 중 이 부분의 구현은 다음 그림에 나와 있습니다.

.. kernel-figure:: TreeRCU-callback-registry.svg

Because ``call_rcu()`` normally acts only on CPU-local state, it
provides no ordering guarantees, either for itself or for phase one of
the update (which again will usually be removal of an element from an
RCU-protected data structure). It simply enqueues the ``rcu_head``
structure on a per-CPU list, which cannot become associated with a grace
period until a later call to ``rcu_accelerate_cbs()``, as shown in the
diagram above.

One set of code paths shown on the left invokes ``rcu_accelerate_cbs()``
via ``note_gp_changes()``, either directly from ``call_rcu()`` (if the
current CPU is inundated with queued ``rcu_head`` structures) or more
likely from an ``RCU_SOFTIRQ`` handler. Another code path in the middle
is taken only in kernels built with ``CONFIG_RCU_FAST_NO_HZ=y``, which
invokes ``rcu_accelerate_cbs()`` via ``rcu_prepare_for_idle()``. The
final code path on the right is taken only in kernels built with
``CONFIG_HOTPLUG_CPU=y``, which invokes ``rcu_accelerate_cbs()`` via
``rcu_advance_cbs()``, ``rcu_migrate_callbacks``,
``rcutree_migrate_callbacks()``, and ``takedown_cpu()``, which in turn
is invoked on a surviving CPU after the outgoing CPU has been completely
offlined.

There are a few other code paths within grace-period processing that
opportunistically invoke ``rcu_accelerate_cbs()``. However, either way,
all of the CPU's recently queued ``rcu_head`` structures are associated
with a future grace-period number under the protection of the CPU's lead
``rcu_node`` structure's ``->lock``. In all cases, there is full
ordering against any prior critical section for that same ``rcu_node``
structure's ``->lock``, and also full ordering against any of the
current task's or CPU's prior critical sections for any ``rcu_node``
structure's ``->lock``.

The next section will show how this ordering ensures that any accesses
prior to the ``call_rcu()`` (particularly including phase one of the
update) happen before the start of the corresponding grace period.

``call_rcu()`` 는 일반적으로 CPU 로컬 상태에서만 작동하기 때문에 자체 
또는 업데이트의 1단계(일반적으로 RCU 보호 데이터 struct에서 요소를 
제거함)에 대한 순서 보장을 제공하지 않습니다. 위의 다이어그램에 표시된 
것처럼 나중에 ``rcu_accelerate_cbs()`` 를 호출할 때까지 유예 기간과 
연결될 수 없는 CPU당 목록에 ``rcu_head`` struct를 큐에 넣기만 하면 됩니다.

왼쪽에 표시된 한 세트의 코드 경로는 ``note_gp_changes()`` 를 통해 
``rcu_accelerate_cbs()`` 를 호출합니다. ``call_rcu()`` 에서 직접 
호출하거나(현재 CPU가 대기 중인 ``rcu_head`` struct로 넘쳐나는 경우) 
RCU_SOFTIRQ 핸들러에서 더 가능성이 높습니다. 중간에 있는 다른 코드 
경로는 ``rcu_prepare_for_idle()`` 을 통해 ``rcu_accelerate_cbs()`` 를 
호출하는 CONFIG_RCU_FAST_NO_HZ=y로 빌드된 커널에서만 사용됩니다. 
오른쪽의 마지막 코드 경로는 CONFIG_HOTPLUG_CPU=y로 빌드된 커널에서만 
가져옵니다. 이 커널은 ``rcu_advance_cbs()``, ``rcu_migrate_callbacks``, 
``rcutree_migrate_callbacks()`` 및 ``takedown_cpu()`` 를 통해 
``rcu_accelerate_cbs()`` 를 호출하고 나가는 CPU가 완전히 오프라인된 
후 남아 있는 CPU에서 차례로 호출됩니다.

기회에 따라 ``rcu_accelerate_cbs()`` 를 호출하는 유예 기간 처리 
내에 몇 가지 다른 코드 경로가 있습니다. 그러나 어느 쪽이든 CPU의 
최근 대기열에 있는 모든 ``rcu_head`` struct는 CPU의 리드 ``rcu_node`` 
struct의 ``->lock`` 보호 아래 미래의 유예 기간 번호와 연결됩니다. 모든 
경우에 동일한 ``rcu_node`` struct의 ``->lock`` 에 대한 이전 임계 섹션에 
대한 전체 순서가 있고, ``rcu_node`` struct의 ``->lock`` 에 대한 현재 
태스크 또는 CPU의 이전 임계 섹션에 대한 전체 순서도 있습니다.

다음 섹션에서는 이 순서가 ``call_rcu()`` 이전의 모든 액세스(특히 
업데이트의 1단계 포함)가 해당 유예 기간이 시작되기 전에 발생하도록 
보장하는 방법을 보여줍니다.

+-----------------------------------------------------------------------+
| **Quick Quiz**:                                                       |
+-----------------------------------------------------------------------+
| But what about ``synchronize_rcu()``?                                 |
+-----------------------------------------------------------------------+
| **Answer**:                                                           |
+-----------------------------------------------------------------------+
| The ``synchronize_rcu()`` passes ``call_rcu()`` to ``wait_rcu_gp()``, |
| which invokes it. So either way, it eventually comes down to          |
| ``call_rcu()``.                                                       |
|                                                                       |
| ``synchronize_rcu()`` 는 호출하는 ``wait_rcu_gp()`` 에                |
| ``call_rcu()`` 를 전달합니다. 어느 쪽이든 결국 ``call_rcu()`` 로      |
| 귀결됩니다.                                                           |
+-----------------------------------------------------------------------+

Grace-Period Initialization
^^^^^^^^^^^^^^^^^^^^^^^^^^^

Grace-period initialization is carried out by the grace-period kernel
thread, which makes several passes over the ``rcu_node`` tree within the
``rcu_gp_init()`` function. This means that showing the full flow of
ordering through the grace-period computation will require duplicating
this tree. If you find this confusing, please note that the state of the
``rcu_node`` changes over time, just like Heraclitus's river. However,
to keep the ``rcu_node`` river tractable, the grace-period kernel
thread's traversals are presented in multiple parts, starting in this
section with the various phases of grace-period initialization.

The first ordering-related grace-period initialization action is to
advance the ``rcu_state`` structure's ``->gp_seq`` grace-period-number
counter, as shown below:

유예 기간 초기화는 ``rcu_gp_init()`` 함수 내에서 ``rcu_node`` 트리를 
여러 번 통과하는 유예 기간 커널 스레드에 의해 수행됩니다. 즉, 유예 
기간 계산을 통해 주문의 전체 흐름을 표시하려면 이 트리를 복제해야 
합니다. 이것이 혼란스럽다면 ``rcu_node`` 의 상태는 헤라클레이토스의 
강처럼 시간이 지남에 따라 변한다는 점에 유의하십시오. 그러나 
``rcu_node`` 강을 다루기 쉽게 유지하기 위해 유예 기간 커널 스레드의 
순회는 이 섹션에서 유예 기간 초기화의 다양한 단계를 시작으로 여러 
부분으로 제공됩니다.

첫 번째 주문 관련 유예 기간 초기화 작업은 아래와 같이 
``rcu_state`` struct의 ``->gp_seq`` 유예 기간 숫자 카운터를 
진행하는 것입니다.

.. kernel-figure:: TreeRCU-gp-init-1.svg

The actual increment is carried out using ``smp_store_release()``, which
helps reject false-positive RCU CPU stall detection. Note that only the
root ``rcu_node`` structure is touched.

The first pass through the ``rcu_node`` tree updates bitmasks based on
CPUs having come online or gone offline since the start of the previous
grace period. In the common case where the number of online CPUs for
this ``rcu_node`` structure has not transitioned to or from zero, this
pass will scan only the leaf ``rcu_node`` structures. However, if the
number of online CPUs for a given leaf ``rcu_node`` structure has
transitioned from zero, ``rcu_init_new_rnp()`` will be invoked for the
first incoming CPU. Similarly, if the number of online CPUs for a given
leaf ``rcu_node`` structure has transitioned to zero,
``rcu_cleanup_dead_rnp()`` will be invoked for the last outgoing CPU.
The diagram below shows the path of ordering if the leftmost
``rcu_node`` structure onlines its first CPU and if the next
``rcu_node`` structure has no online CPUs (or, alternatively if the
leftmost ``rcu_node`` structure offlines its last CPU and if the next
``rcu_node`` structure has no online CPUs).

실제 증분은 ``smp_store_release()`` 를 사용하여 수행되며, 이는 
잘못된 긍정 RCU CPU 스톨 감지를 거부하는 데 도움이 됩니다. 루트 
``rcu_node`` 구조만 건드린다는 점에 유의하십시오.

``rcu_node`` 트리를 통한 첫 번째 패스는 이전 유예 기간이 시작된 이후 
온라인 또는 오프라인이 된 CPU를 기반으로 비트 마스크를 업데이트합니다. 
이 ``rcu_node`` struct에 대한 온라인 CPU 수가 0으로 또는 0에서 
전환되지 않은 일반적인 경우 이 패스는 리프 ``rcu_node`` struct만 
스캔합니다. 그러나 주어진 리프 ``rcu_node`` 구조에 대한 온라인 CPU 
수가 0에서 전환되면 ``rcu_init_new_rnp()`` 가 첫 번째 수신 CPU에 
대해 호출됩니다. 유사하게 주어진 리프 ``rcu_node`` struct에 대한 
온라인 CPU 수가 0으로 전환되면 ``rcu_cleanup_dead_rnp()`` 가 
마지막 나가는 CPU에 대해 호출됩니다.
아래 다이어그램은 가장 왼쪽 ``rcu_node`` struct가 첫 번째 CPU를 
온라인 상태로 만들고 다음 ``rcu_node`` struct에 온라인 CPU가 없는 
경우(또는 맨 왼쪽 ``rcu_node`` struct가 마지막 CPU를 오프라인 
상태로 만들고 다음 ``rcu_node`` struct에 온라인 CPU가 없는 경우) 
순서 지정 경로를 보여줍니다.

.. kernel-figure:: TreeRCU-gp-init-2.svg

The final ``rcu_gp_init()`` pass through the ``rcu_node`` tree traverses
breadth-first, setting each ``rcu_node`` structure's ``->gp_seq`` field
to the newly advanced value from the ``rcu_state`` structure, as shown
in the following diagram.

마지막 ``rcu_gp_init()``는 ``rcu_node`` 트리를 통과하여 너비 우선 순회를 
통해 다음 다이어그램과 같이 각 ``rcu_node`` 구조의 ``->gp_seq`` 필드를 
``rcu_state`` 구조에서 새로 고급 값으로 설정합니다.

.. kernel-figure:: TreeRCU-gp-init-3.svg

This change will also cause each CPU's next call to
``__note_gp_changes()`` to notice that a new grace period has started,
as described in the next section. But because the grace-period kthread
started the grace period at the root (with the advancing of the
``rcu_state`` structure's ``->gp_seq`` field) before setting each leaf
``rcu_node`` structure's ``->gp_seq`` field, each CPU's observation of
the start of the grace period will happen after the actual start of the
grace period.

이 변경으로 인해 각 CPU는 ``__note_gp_changes()`` 에 대한 다음 호출에서 
다음 섹션에 설명된 대로 새로운 유예 기간이 시작되었음을 알립니다. 
그러나 유예 기간 kthread는 각 리프 ``rcu_node`` struct의 ``->gp_seq`` 
필드를 설정하기 전에 루트에서 유예 기간을 시작했기 때문에
(``rcu_state`` struct의 ``->gp_seq`` 필드가 진행됨) 각 CPU의 유예 
기간 시작 관찰은 유예 기간의 실제 시작 이후에 발생합니다.

+-----------------------------------------------------------------------+
| **Quick Quiz**:                                                       |
+-----------------------------------------------------------------------+
| But what about the CPU that started the grace period? Why wouldn't it |
| see the start of the grace period right when it started that grace    |
| period?                                                               |
|                                                                       |
| 그러나 유예 기간을 시작한 CPU는 어떻습니까? 유예 기간이 시작되었을 때 |
| 바로 유예 기간의 시작이 표시되지 않는 이유는 무엇입니까?              |
+-----------------------------------------------------------------------+
| **Answer**:                                                           |
+-----------------------------------------------------------------------+
| In some deep philosophical and overly anthromorphized sense, yes, the |
| CPU starting the grace period is immediately aware of having done so. |
| However, if we instead assume that RCU is not self-aware, then even   |
| the CPU starting the grace period does not really become aware of the |
| start of this grace period until its first call to                    |
| ``__note_gp_changes()``. On the other hand, this CPU potentially gets |
| early notification because it invokes ``__note_gp_changes()`` during  |
| its last ``rcu_gp_init()`` pass through its leaf ``rcu_node``         |
| structure.                                                            |
| 심오한 철학적이고 지나치게 의인화된 의미에서 예, 유예 기간을 시작하는 |
| CPU는 유예 기간이 시작되었음을 즉시 인식합니다. 그러나 대신 RCU가     |
| 자체 인식하지 못한다고 가정하면 유예 기간을 시작하는 CPU도            |
| ``__note_gp_changes()`` 를 처음 호출할 때까지 이 유예 기간의 시작을   |
| 실제로 인식하지 못합니다. 반면에 이 CPU는 마지막 ``rcu_gp_init()`` 가 |
| 리프 ``rcu_node`` struct를 통과하는 동안 ``__note_gp_changes()`` 를   |
| 호출하기 때문에 조기 알림을 받을 가능성이 있습니다.                   |
+-----------------------------------------------------------------------+

Self-Reported Quiescent States
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When all entities that might block the grace period have reported
quiescent states (or as described in a later section, had quiescent
states reported on their behalf), the grace period can end. Online
non-idle CPUs report their own quiescent states, as shown in the
following diagram:

유예 기간을 차단할 수 있는 모든 엔터티가 정지 상태를 보고하면(또는 
이후 섹션에서 설명하는 대로 정지 상태가 대신 보고됨) 유예 기간이 
종료될 수 있습니다. 온라인 비유휴 CPU는 다음 다이어그램과 같이 
자체 정지 상태를 보고합니다.

.. kernel-figure:: TreeRCU-qs.svg

This is for the last CPU to report a quiescent state, which signals the
end of the grace period. Earlier quiescent states would push up the
``rcu_node`` tree only until they encountered an ``rcu_node`` structure
that is waiting for additional quiescent states. However, ordering is
nevertheless preserved because some later quiescent state will acquire
that ``rcu_node`` structure's ``->lock``.

Any number of events can lead up to a CPU invoking ``note_gp_changes``
(or alternatively, directly invoking ``__note_gp_changes()``), at which
point that CPU will notice the start of a new grace period while holding
its leaf ``rcu_node`` lock. Therefore, all execution shown in this
diagram happens after the start of the grace period. In addition, this
CPU will consider any RCU read-side critical section that started before
the invocation of ``__note_gp_changes()`` to have started before the
grace period, and thus a critical section that the grace period must
wait on.

이것은 유예 기간의 끝을 알리는 정지 상태를 보고하는 마지막 CPU를 위한 
것입니다. 이전의 정지 상태는 추가 정지 상태를 기다리고 있는 
``rcu_node`` struct를 만날 때까지만 ``rcu_node`` 트리를 푸시합니다. 
그러나 이후의 정지 상태에서 ``rcu_node`` 구조의 ``->lock`` 을 획득하기 
때문에 순서는 유지됩니다.

CPU가 ``note_gp_changes`` (또는 ``__note_gp_changes()`` 를 직접 호출)를 
호출할 수 있는 이벤트가 얼마든지 있을 수 있으며, 이 시점에서 CPU는 리프 
``rcu_node`` 잠금을 유지하면서 새로운 유예 기간의 시작을 알 수 있습니다. 
따라서 이 다이어그램에 표시된 모든 실행은 유예 기간이 시작된 후에 
발생합니다. 또한 이 CPU는 ``__note_gp_changes()`` 호출 전에 시작된 모든 
RCU 읽기 측 임계 섹션을 유예 기간 전에 시작된 것으로 간주하므로 유예 
기간이 기다려야 하는 임계 섹션입니다.

+-----------------------------------------------------------------------+
| **Quick Quiz**:                                                       |
+-----------------------------------------------------------------------+
| But a RCU read-side critical section might have started after the     |
| beginning of the grace period (the advancing of ``->gp_seq`` from     |
| earlier), so why should the grace period wait on such a critical      |
| section?                                                              |
|                                                                       |
| 그러나 RCU 읽기 측 임계 영역은 유예 기간이 시작된 후에 시작되었을 수  |
| 있습니다(이전보다 ``->gp_seq`` 가 앞당겨짐). 그렇다면 유예 기간이     |
| 그러한 임계 영역에서 기다려야 하는 이유는 무엇입니까?                 |
+-----------------------------------------------------------------------+
| **Answer**:                                                           |
+-----------------------------------------------------------------------+
| It is indeed not necessary for the grace period to wait on such a     |
| critical section. However, it is permissible to wait on it. And it is |
| furthermore important to wait on it, as this lazy approach is far     |
| more scalable than a “big bang” all-at-once grace-period start could  |
| possibly be.                                                          |
|                                                                       |
| 유예 기간이 그러한 중요한 섹션을 기다리는 것은 실제로 필요하지        |
| 않습니다. 그러나 기다리는 것은 허용됩니다. 그리고 이 게으른 접근      |
| 방식은 한 번에 "빅뱅" 유예 기간 시작이 가능할 수 있는 것보다 훨씬 더  |
| 확장 가능하기 때문에 기다리는 것이 더 중요합니다.                     |
+-----------------------------------------------------------------------+

If the CPU does a context switch, a quiescent state will be noted by
``rcu_note_context_switch()`` on the left. On the other hand, if the CPU
takes a scheduler-clock interrupt while executing in usermode, a
quiescent state will be noted by ``rcu_sched_clock_irq()`` on the right.
Either way, the passage through a quiescent state will be noted in a
per-CPU variable.

The next time an ``RCU_SOFTIRQ`` handler executes on this CPU (for
example, after the next scheduler-clock interrupt), ``rcu_core()`` will
invoke ``rcu_check_quiescent_state()``, which will notice the recorded
quiescent state, and invoke ``rcu_report_qs_rdp()``. If
``rcu_report_qs_rdp()`` verifies that the quiescent state really does
apply to the current grace period, it invokes ``rcu_report_rnp()`` which
traverses up the ``rcu_node`` tree as shown at the bottom of the
diagram, clearing bits from each ``rcu_node`` structure's ``->qsmask``
field, and propagating up the tree when the result is zero.

Note that traversal passes upwards out of a given ``rcu_node`` structure
only if the current CPU is reporting the last quiescent state for the
subtree headed by that ``rcu_node`` structure. A key point is that if a
CPU's traversal stops at a given ``rcu_node`` structure, then there will
be a later traversal by another CPU (or perhaps the same one) that
proceeds upwards from that point, and the ``rcu_node`` ``->lock``
guarantees that the first CPU's quiescent state happens before the
remainder of the second CPU's traversal. Applying this line of thought
repeatedly shows that all CPUs' quiescent states happen before the last
CPU traverses through the root ``rcu_node`` structure, the “last CPU”
being the one that clears the last bit in the root ``rcu_node``
structure's ``->qsmask`` field.

CPU가 컨텍스트 전환을 수행하면 왼쪽의 ``rcu_note_context_switch()`` 에
의해 정지 상태가 표시됩니다. 반면에 CPU가 사용자 모드에서 실행하는 동안 
스케줄러-클록 인터럽트를 받으면 오른쪽의 ``rcu_sched_clock_irq()`` 에 
의해 정지 상태가 표시됩니다.
어느 쪽이든 정지 상태를 통과하는 과정은 CPU별 변수에 기록됩니다.

다음에 ``RCU_SOFTIRQ`` 핸들러가 이 CPU에서 실행될 때(예를 들어 다음 
스케줄러 클럭 인터럽트 이후) ``rcu_core()`` 는 
``rcu_check_quiescent_state()`` 를 호출하여 기록된 정지 상태를 인식하고
``rcu_report_qs_rdp()`` 를 호출합니다. ``rcu_report_qs_rdp()`` 가 
정지 상태가 현재 유예 기간에 실제로 적용되는지 확인하면 
``rcu_report_rnp()`` 를 호출하여 다이어그램 하단에 표시된 ``rcu_node`` 
트리를 순회하고 각 ``rcu_node`` struct의 ``->qsmask`` 필드에서 비트를 
지우고 결과가 0일 때 트리를 전파합니다.

순회는 현재 CPU가 해당 ``rcu_node`` struct가 이끄는 하위 트리의 
마지막 정지 상태를 보고하는 경우에만 주어진 ``rcu_node`` struct 밖으로 
위쪽으로 전달됩니다. 핵심은 CPU의 순회가 주어진 ``rcu_node`` struct에서 
멈추면 그 지점에서 위쪽으로 진행하는 다른 CPU(또는 아마도 동일한 CPU)에 
의한 이후 순회가 있을 것이며 ``rcu_node`` ``->lock`` 은 첫 번째 
CPU의 정지 상태가 나머지 두 번째 CPU의 순회 전에 발생하도록 보장한다는 
것입니다. 이 생각을 반복적으로 적용하면 마지막 CPU가 루트 
``rcu_node`` struct를 통과하기 전에 모든 CPU의 정지 상태가 발생하며 
"마지막 CPU"는 루트 ``rcu_node`` struct의 ``->qsmask`` 필드에서 마지막 
비트를 지우는 것입니다.

Dynamic Tick Interface
^^^^^^^^^^^^^^^^^^^^^^

Due to energy-efficiency considerations, RCU is forbidden from
disturbing idle CPUs. CPUs are therefore required to notify RCU when
entering or leaving idle state, which they do via fully ordered
value-returning atomic operations on a per-CPU variable. The ordering
effects are as shown below:

에너지 효율성 고려 사항으로 인해 RCU는 유휴 CPU를 방해하는 것이 
금지됩니다. 따라서 CPU는 유휴 상태에 들어가거나 나올 때 RCU에 알려야 
하며, 이는 CPU당 변수에 대해 완전하게 정렬된 값 반환 원자 연산을 
통해 수행합니다. 주문 효과는 다음과 같습니다.

.. kernel-figure:: TreeRCU-dyntick.svg

The RCU grace-period kernel thread samples the per-CPU idleness variable
while holding the corresponding CPU's leaf ``rcu_node`` structure's
``->lock``. This means that any RCU read-side critical sections that
precede the idle period (the oval near the top of the diagram above)
will happen before the end of the current grace period. Similarly, the
beginning of the current grace period will happen before any RCU
read-side critical sections that follow the idle period (the oval near
the bottom of the diagram above).

Plumbing this into the full grace-period execution is described
`below <Forcing Quiescent States_>`__.

RCU 유예 기간 커널 스레드는 해당 CPU의 리프 ``rcu_node`` 구조의 
``->lock`` 을 유지하면서 CPU별 유휴 변수를 샘플링합니다. 즉, 유휴 
기간(위 다이어그램 상단 근처의 타원) 이전의 모든 RCU 읽기 측 임계 
섹션은 현재 유예 기간이 끝나기 전에 발생합니다. 마찬가지로, 현재 
유예 기간의 시작은 유휴 기간(위 다이어그램 하단 근처의 타원)을 
따르는 RCU 읽기 측 임계 섹션 이전에 발생합니다.

이것을 전체 유예 기간 실행으로 연결하는 것은 
`아래 <Forcing Quiescent States_>` 에 설명되어 있습니다__.

CPU-Hotplug Interface
^^^^^^^^^^^^^^^^^^^^^

RCU is also forbidden from disturbing offline CPUs, which might well be
powered off and removed from the system completely. CPUs are therefore
required to notify RCU of their comings and goings as part of the
corresponding CPU hotplug operations. The ordering effects are shown
below:

RCU는 또한 전원이 꺼지고 시스템에서 완전히 제거될 수 있는 오프라인 
CPU를 방해하는 것도 금지됩니다. 따라서 CPU는 해당 CPU 핫플러그 작업의 
일부로 RCU에 들어오고 나가는 것을 알려야 합니다. 주문 효과는 다음과 같습니다.

.. kernel-figure:: TreeRCU-hotplug.svg

Because CPU hotplug operations are much less frequent than idle
transitions, they are heavier weight, and thus acquire the CPU's leaf
``rcu_node`` structure's ``->lock`` and update this structure's
``->qsmaskinitnext``. The RCU grace-period kernel thread samples this
mask to detect CPUs having gone offline since the beginning of this
grace period.

Plumbing this into the full grace-period execution is described
`below <Forcing Quiescent States_>`__.

CPU 핫플러그 작업은 유휴 전환보다 훨씬 덜 빈번하기 때문에 더 무겁고 
따라서 CPU의 리프 ``rcu_node`` 구조의 ``->lock`` 을 획득하고 이 구조의 
``->qsmaskinitnext`` 를 업데이트합니다. RCU 유예 기간 커널 스레드는 
이 유예 기간이 시작된 이후 오프라인이 된 CPU를 감지하기 위해 이 
마스크를 샘플링합니다.

이것을 전체 유예 기간 실행으로 연결하는 것은 
`아래 <Forcing Quiescent States_>` 에 설명되어 있습니다__.

Forcing Quiescent States
^^^^^^^^^^^^^^^^^^^^^^^^

As noted above, idle and offline CPUs cannot report their own quiescent
states, and therefore the grace-period kernel thread must do the
reporting on their behalf. This process is called “forcing quiescent
states”, it is repeated every few jiffies, and its ordering effects are
shown below:

위에서 언급한 것처럼 유휴 및 오프라인 CPU는 자신의 정지 상태를 보고할 수 
없으므로 유예 기간 커널 스레드가 대신 보고를 수행해야 합니다. 이 
프로세스를 "정지 상태 강제 실행"이라고 하며 몇 초 간격으로 반복되며 
순서 지정 효과는 다음과 같습니다.

.. kernel-figure:: TreeRCU-gp-fqs.svg

Each pass of quiescent state forcing is guaranteed to traverse the leaf
``rcu_node`` structures, and if there are no new quiescent states due to
recently idled and/or offlined CPUs, then only the leaves are traversed.
However, if there is a newly offlined CPU as illustrated on the left or
a newly idled CPU as illustrated on the right, the corresponding
quiescent state will be driven up towards the root. As with
self-reported quiescent states, the upwards driving stops once it
reaches an ``rcu_node`` structure that has quiescent states outstanding
from other CPUs.

정지 상태 강제 적용의 각 패스는 리프 ``rcu_node`` 구조를 통과하도록 
보장되며, 최근 유휴 및/또는 오프라인 CPU로 인해 새로운 정지 상태가 
없으면 리프만 통과합니다.
그러나 왼쪽 그림과 같이 새로 오프라인된 CPU 또는 오른쪽 그림과 같이 새로 
유휴 상태인 CPU가 있는 경우 해당 quiescent 상태가 루트를 향해 구동됩니다. 
자체 보고된 정지 상태와 마찬가지로 다른 CPU에서 뛰어난 정지 상태를 가진 
``rcu_node`` 구조에 도달하면 위쪽으로의 구동이 중지됩니다.

+-----------------------------------------------------------------------+
| **Quick Quiz**:                                                       |
+-----------------------------------------------------------------------+
| The leftmost drive to root stopped before it reached the root         |
| ``rcu_node`` structure, which means that there are still CPUs         |
| subordinate to that structure on which the current grace period is    |
| waiting. Given that, how is it possible that the rightmost drive to   |
| root ended the grace period?                                          |
|                                                                       |
| 루트에 대한 가장 왼쪽 드라이브는 루트 ``rcu_node`` 구조에 도달하기    |
| 전에 중지되었습니다. 이는 현재 유예 기간이 대기 중인 해당 구조에      |
| 종속된 CPU가 여전히 있음을 의미합니다. 그렇다면 루트에 대한 가장      |
| 오른쪽 드라이브가 유예 기간을 끝낼 수 있는 방법은 무엇입니까?         |
+-----------------------------------------------------------------------+
| **Answer**:                                                           |
+-----------------------------------------------------------------------+
| Good analysis! It is in fact impossible in the absence of bugs in     |
| RCU. But this diagram is complex enough as it is, so simplicity       |
| overrode accuracy. You can think of it as poetic license, or you can  |
| think of it as misdirection that is resolved in the                   |
| `stitched-together diagram <Putting It All Together_>`__.             |
|                                                                       |
| 좋은 분석! 사실 RCU에 버그가 없으면 불가능합니다. 그러나 이           |
| 다이어그램은 그 자체로 충분히 복잡하므로 단순성이 정확성을            |
| 압도합니다. 시적 면허라고 생각할 수도 있고,                           |
| `접합도 <Putting It All Together_>`에서 해결되는 잘못된 방향이라고    |
| 생각할 수도 있습니다__.                                               |
+-----------------------------------------------------------------------+

Grace-Period Cleanup
^^^^^^^^^^^^^^^^^^^^

Grace-period cleanup first scans the ``rcu_node`` tree breadth-first
advancing all the ``->gp_seq`` fields, then it advances the
``rcu_state`` structure's ``->gp_seq`` field. The ordering effects are
shown below:

유예 기간 정리는 먼저 ``rcu_node`` 트리 너비 우선을 스캔하여 모든 
``->gp_seq`` 필드를 진행한 다음 ``rcu_state`` 구조의 ``->gp_seq`` 필드를 
진행합니다. 주문 효과는 다음과 같습니다.

.. kernel-figure:: TreeRCU-gp-cleanup.svg

As indicated by the oval at the bottom of the diagram, once grace-period
cleanup is complete, the next grace period can begin.

다이어그램 하단의 타원으로 표시된 것처럼 유예 기간 정리가 완료되면 다음 
유예 기간이 시작될 수 있습니다.

+-----------------------------------------------------------------------+
| **Quick Quiz**:                                                       |
+-----------------------------------------------------------------------+
| But when precisely does the grace period end?                         |
|                                                                       |
| 그러나 유예 기간은 정확히 언제 종료됩니까?                            |
+-----------------------------------------------------------------------+
| **Answer**:                                                           |
+-----------------------------------------------------------------------+
| There is no useful single point at which the grace period can be said |
| to end. The earliest reasonable candidate is as soon as the last CPU  |
| has reported its quiescent state, but it may be some milliseconds     |
| before RCU becomes aware of this. The latest reasonable candidate is  |
| once the ``rcu_state`` structure's ``->gp_seq`` field has been        |
| updated, but it is quite possible that some CPUs have already         |
| completed phase two of their updates by that time. In short, if you   |
| are going to work with RCU, you need to learn to embrace uncertainty. |
|                                                                       |
| 유예 기간이 끝났다고 말할 수 있는 유용한 단일 지점은 없습니다.        |
| 가장 빠른 합리적인 후보는 마지막 CPU가 정지 상태를 보고하는           |
| 즉시이지만 RCU가 이를 인식하기까지는 몇 밀리초가 소요될 수 있습니다.  |
| 가장 최근의 합리적인 후보는 일단 rcu_state 구조의 ->gp_seq 필드가     |
| 업데이트된 경우이지만 일부 CPU는 이미 그 시점까지 업데이트의 2단계를  |
| 완료했을 가능성이 큽니다. 요컨대 RCU와 함께 일하려면 불확실성을       |
| 포용하는 법을 배워야 합니다.                                          |
+-----------------------------------------------------------------------+

Callback Invocation
^^^^^^^^^^^^^^^^^^^

Once a given CPU's leaf ``rcu_node`` structure's ``->gp_seq`` field has
been updated, that CPU can begin invoking its RCU callbacks that were
waiting for this grace period to end. These callbacks are identified by
``rcu_advance_cbs()``, which is usually invoked by
``__note_gp_changes()``. As shown in the diagram below, this invocation
can be triggered by the scheduling-clock interrupt
(``rcu_sched_clock_irq()`` on the left) or by idle entry
(``rcu_cleanup_after_idle()`` on the right, but only for kernels build
with ``CONFIG_RCU_FAST_NO_HZ=y``). Either way, ``RCU_SOFTIRQ`` is
raised, which results in ``rcu_do_batch()`` invoking the callbacks,
which in turn allows those callbacks to carry out (either directly or
indirectly via wakeup) the needed phase-two processing for each update.

주어진 CPU의 리프 ``rcu_node`` 구조의 ``->gp_seq`` 필드가 업데이트되면 
해당 CPU는 이 유예 기간이 끝나기를 기다리고 있던 RCU 콜백 호출을 시작할 
수 있습니다. 이러한 콜백은 ``rcu_advance_cbs()`` 에 의해 식별되며 
일반적으로 ``__note_gp_changes()`` 에 의해 호출됩니다. 
아래 다이어그램에서 볼 수 있듯이 이 호출은 스케줄링 클럭 
인터럽트(왼쪽의 ``rcu_sched_clock_irq()``) 또는 유휴 항목(오른쪽의 
``rcu_cleanup_after_idle()`` 에 의해 트리거될 수 있지만 
``CONFIG_RCU_FAST_NO_HZ=y`` 로 빌드된 커널에만 해당됩니다. 어느 쪽이든 
``RCU_SOFTIRQ`` 가 발생하여 ``rcu_do_batch()`` 가 콜백을 호출하고, 
콜백이 각 업데이트에 필요한 2단계 처리를 (직접 또는 깨우기를 통해 
간접적으로) 수행할 수 있습니다.

.. kernel-figure:: TreeRCU-callback-invocation.svg

Please note that callback invocation can also be prompted by any number
of corner-case code paths, for example, when a CPU notes that it has
excessive numbers of callbacks queued. In all cases, the CPU acquires
its leaf ``rcu_node`` structure's ``->lock`` before invoking callbacks,
which preserves the required ordering against the newly completed grace
period.

However, if the callback function communicates to other CPUs, for
example, doing a wakeup, then it is that function's responsibility to
maintain ordering. For example, if the callback function wakes up a task
that runs on some other CPU, proper ordering must in place in both the
callback function and the task being awakened. To see why this is
important, consider the top half of the `grace-period
cleanup`_ diagram. The callback might be
running on a CPU corresponding to the leftmost leaf ``rcu_node``
structure, and awaken a task that is to run on a CPU corresponding to
the rightmost leaf ``rcu_node`` structure, and the grace-period kernel
thread might not yet have reached the rightmost leaf. In this case, the
grace period's memory ordering might not yet have reached that CPU, so
again the callback function and the awakened task must supply proper
ordering.

예를 들어, CPU가 대기 중인 콜백 수가 너무 많다는 것을 알게 되는 경우와 
같이 콜백 호출은 코너 케이스 코드 경로의 수에 관계없이 프롬프트될 수 
있습니다. 모든 경우에 CPU는 콜백을 호출하기 전에 리프 
``rcu_node`` struct의 ``->lock`` 을 획득하여 새로 완료된 유예 기간에 
대해 필요한 순서를 유지합니다.

그러나 콜백 함수가 다른 CPU와 통신하는 경우(예: 웨이크업 수행) 순서를 
유지하는 것은 해당 함수의 책임입니다. 예를 들어 콜백 함수가 다른 
CPU에서 실행되는 작업을 깨우는 경우 콜백 함수와 깨어나는 작업 모두에 
적절한 순서가 있어야 합니다. 이것이 왜 중요한지 알아보려면 
`grace-period cleanup`_ 다이어그램의 상단 절반을 고려하십시오. 
콜백은 가장 왼쪽 잎 ``rcu_node`` struct에 해당하는 CPU에서 실행 중일
수 있으며 가장 오른쪽 잎 ``rcu_node`` struct에 해당하는 CPU에서 
실행할 작업을 깨울 수 있으며 유예 기간 커널 스레드는 아직 가장 오른쪽 
잎에 도달하지 않았을 수 있습니다. 이 경우 유예 기간의 메모리 순서가 
아직 해당 CPU에 도달하지 않았을 수 있으므로 다시 콜백 함수와 깨어난 
작업이 적절한 순서를 제공해야 합니다.

Putting It All Together
~~~~~~~~~~~~~~~~~~~~~~~

A stitched-together diagram is here:

.. kernel-figure:: TreeRCU-gp.svg

다이어그램 홈페이지
https://www.kernel.org/doc/Documentation/RCU/Design/Memory-Ordering/Tree-RCU-Diagram.html

Legal Statement
~~~~~~~~~~~~~~~

This work represents the view of the author and does not necessarily
represent the view of IBM.

Linux is a registered trademark of Linus Torvalds.

Other company, product, and service names may be trademarks or service
marks of others.
