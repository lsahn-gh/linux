.. _whatisrcu_doc:

What is RCU?  --  "Read, Copy, Update"
======================================

Please note that the "What is RCU?" LWN series is an excellent place
to start learning about RCU:

| 1.	What is RCU, Fundamentally?  http://lwn.net/Articles/262464/
| 2.	What is RCU? Part 2: Usage   http://lwn.net/Articles/263130/
| 3.	RCU part 3: the RCU API      http://lwn.net/Articles/264090/
| 4.	The RCU API, 2010 Edition    http://lwn.net/Articles/418853/
| 	2010 Big API Table           http://lwn.net/Articles/419086/
| 5.	The RCU API, 2014 Edition    http://lwn.net/Articles/609904/
|	2014 Big API Table           http://lwn.net/Articles/609973/


What is RCU?

RCU is a synchronization mechanism that was added to the Linux kernel
during the 2.5 development effort that is optimized for read-mostly
situations.  Although RCU is actually quite simple once you understand it,
getting there can sometimes be a challenge.  Part of the problem is that
most of the past descriptions of RCU have been written with the mistaken
assumption that there is "one true way" to describe RCU.  Instead,
the experience has been that different people must take different paths
to arrive at an understanding of RCU.  This document provides several
different paths, as follows:

RCU는 대부분 읽기 상황에 최적화된 2.5 개발 노력 중에 Linux 커널에 추가된
동기화 메커니즘입니다. RCU는 일단 이해하고 나면 실제로 매우 간단하지만,
거기에 도달하는 것은 때때로 어려울 수 있습니다. 문제의 일부는 RCU에 대한
대부분의 과거 설명이 RCU를 설명하는 하나의 진정한 방법이 있다는 잘못된
가정으로 작성되었다는 것입니다. 그 대신 RCU를 이해하기 위해 서로 다른
사람들이 서로 다른 경로를 택해야 한다는 경험이 있었습니다. 이 문서는
다음과 같은 여러 경로를 제공합니다.

:ref:`1.	RCU OVERVIEW <1_whatisRCU>`

:ref:`2.	WHAT IS RCU'S CORE API? <2_whatisRCU>`

:ref:`3.	WHAT ARE SOME EXAMPLE USES OF CORE RCU API? <3_whatisRCU>`

:ref:`4.	WHAT IF MY UPDATING THREAD CANNOT BLOCK? <4_whatisRCU>`

:ref:`5.	WHAT ARE SOME SIMPLE IMPLEMENTATIONS OF RCU? <5_whatisRCU>`

:ref:`6.	ANALOGY WITH READER-WRITER LOCKING <6_whatisRCU>`

:ref:`7.	FULL LIST OF RCU APIs <7_whatisRCU>`

:ref:`8.	ANSWERS TO QUICK QUIZZES <8_whatisRCU>`

People who prefer starting with a conceptual overview should focus on
Section 1, though most readers will profit by reading this section at
some point.  People who prefer to start with an API that they can then
experiment with should focus on Section 2.  People who prefer to start
with example uses should focus on Sections 3 and 4.  People who need to
understand the RCU implementation should focus on Section 5, then dive
into the kernel source code.  People who reason best by analogy should
focus on Section 6.  Section 7 serves as an index to the docbook API
documentation, and Section 8 is the traditional answer key.

So, start with the section that makes the most sense to you and your
preferred method of learning.  If you need to know everything about
everything, feel free to read the whole thing -- but if you are really
that type of person, you have perused the source code and will therefore
never need this document anyway.  ;-)

개념적 개요로 시작하는 것을 선호하는 사람들은 섹션 1에 집중해야 하지만
대부분의 독자는 이 섹션을 어느 시점에서 읽으면 도움이 될 것입니다.
실험할 수 있는 API로 시작하는 것을 선호하는 사람들은 섹션 2에 집중해야
합니다. 예제 사용으로 시작하는 것을 선호하는 사람들은 섹션 3과
4에 집중해야 합니다. RCU 구현을 이해해야 하는 사람들은 섹션 5에 집중해야
합니다. 그런 다음 커널 소스 코드를 살펴보십시오. 유추로 가장 잘 추론하는
사람은 섹션 6에 집중해야 합니다. 섹션 7은 docbook API 문서에 대한 색인
역할을 하며 섹션 8은 전통적인 답변 키입니다.

따라서 귀하에게 가장 적합한 섹션과 선호하는 학습 방법부터 시작하십시오.
모든 것에 대해 모든 것을 알아야 하는 경우 전체 내용을 자유롭게 읽으십시오.
그러나 실제로 그러한 유형의 사람이라면 소스 코드를 정독했으므로 어쨌든
이 문서가 필요하지 않을 것입니다. ;-).

.. _1_whatisRCU:

1.  RCU OVERVIEW
----------------

The basic idea behind RCU is to split updates into "removal" and
"reclamation" phases.  The removal phase removes references to data items
within a data structure (possibly by replacing them with references to
new versions of these data items), and can run concurrently with readers.
The reason that it is safe to run the removal phase concurrently with
readers is the semantics of modern CPUs guarantee that readers will see
either the old or the new version of the data structure rather than a
partially updated reference.  The reclamation phase does the work of reclaiming
(e.g., freeing) the data items removed from the data structure during the
removal phase.  Because reclaiming data items can disrupt any readers
concurrently referencing those data items, the reclamation phase must
not start until readers no longer hold references to those data items.

Splitting the update into removal and reclamation phases permits the
updater to perform the removal phase immediately, and to defer the
reclamation phase until all readers active during the removal phase have
completed, either by blocking until they finish or by registering a
callback that is invoked after they finish.  Only readers that are active
during the removal phase need be considered, because any reader starting
after the removal phase will be unable to gain a reference to the removed
data items, and therefore cannot be disrupted by the reclamation phase.

So the typical RCU update sequence goes something like the following:

a.	Remove pointers to a data structure, so that subsequent
	readers cannot gain a reference to it.

b.	Wait for all previous readers to complete their RCU read-side
	critical sections.

c.	At this point, there cannot be any readers who hold references
	to the data structure, so it now may safely be reclaimed
	(e.g., kfree()d).

Step (b) above is the key idea underlying RCU's deferred destruction.
The ability to wait until all readers are done allows RCU readers to
use much lighter-weight synchronization, in some cases, absolutely no
synchronization at all.  In contrast, in more conventional lock-based
schemes, readers must use heavy-weight synchronization in order to
prevent an updater from deleting the data structure out from under them.
This is because lock-based updaters typically update data items in place,
and must therefore exclude readers.  In contrast, RCU-based updaters
typically take advantage of the fact that writes to single aligned
pointers are atomic on modern CPUs, allowing atomic insertion, removal,
and replacement of data items in a linked structure without disrupting
readers.  Concurrent RCU readers can then continue accessing the old
versions, and can dispense with the atomic operations, memory barriers,
and communications cache misses that are so expensive on present-day
SMP computer systems, even in absence of lock contention.

In the three-step procedure shown above, the updater is performing both
the removal and the reclamation step, but it is often helpful for an
entirely different thread to do the reclamation, as is in fact the case
in the Linux kernel's directory-entry cache (dcache).  Even if the same
thread performs both the update step (step (a) above) and the reclamation
step (step (c) above), it is often helpful to think of them separately.
For example, RCU readers and updaters need not communicate at all,
but RCU provides implicit low-overhead communication between readers
and reclaimers, namely, in step (b) above.

So how the heck can a reclaimer tell when a reader is done, given
that readers are not doing any sort of synchronization operations???
Read on to learn about how RCU's API makes this easy.

RCU의 기본 아이디어는 업데이트를 제거 및 회수 단계로 나누는 것입니다.
제거 단계는 데이터 구조 내에서 데이터 항목에 대한 참조를
제거하고(이러한 데이터 항목의 새 버전에 대한 참조로 교체 가능)
판독기와 동시에 실행할 수 있습니다.
판독기와 동시에 제거 단계를 실행하는 것이 안전한 이유는 최신 CPU의 의미
체계에서 판독기가 부분적으로 업데이트된 참조가 아닌 데이터 구조의 이전
버전 또는 새 버전을 볼 수 있도록 보장하기 때문입니다. 회수 단계는 제거
단계 동안 데이터 구조에서 제거된 데이터 항목을 회수(예: 해제)하는 작업을
수행합니다. 데이터 항목을 회수하면 해당 데이터 항목을 동시에 참조하는
판독기가 중단될 수 있으므로 판독기가 해당 데이터 항목에 대한 참조를 더
이상 보유하지 않을 때까지 회수 단계를 시작해서는 안 됩니다.

업데이트를 제거 및 재확보 단계로 분할하면 업데이터가 제거 단계를 즉시
수행하고 제거 단계 동안 활성화된 모든 판독기가 완료될 때까지 재확보
단계를 연기할 수 있습니다. 그들은 끝납니다. 제거 단계 이후에 시작하는
판독기는 제거된 데이터 항목에 대한 참조를 얻을 수 없으므로 회수 단계에서
중단될 수 없기 때문에 제거 단계 중에 활성화된 판독기만 고려해야 합니다.

따라서 일반적인 RCU 업데이트 순서는 다음과 같습니다.

a. 후속 판독기가 참조를 얻을 수 없도록 데이터 구조에 대한 포인터를
제거합니다.

b. 이전의 모든 판독기가 RCU 읽기 측 중요 섹션을 완료할 때까지 기다리십시오.

c. 이 시점에서 데이터 구조에 대한 참조를 보유하는 판독기가 있을 수
없으므로 이제 안전하게 회수할 수 있습니다(예: kfree()d).

위의 (b) 단계는 RCU의 지연 파기의 핵심 아이디어입니다.
모든 판독기가 완료될 때까지 기다릴 수 있는 기능을 통해 RCU 판독기는
훨씬 더 가벼운 동기화를 사용할 수 있으며 경우에 따라 동기화가 전혀
필요하지 않습니다. 대조적으로, 보다 일반적인 잠금 기반 방식에서는
업데이트 프로그램이 데이터 구조를 삭제하지 못하도록 독자가 강력한
동기화를 사용해야 합니다.
이는 잠금 기반 업데이터가 일반적으로 데이터 항목을 제자리에서 
업데이트하므로 판독기를 제외해야 하기 때문입니다. 반대로 RCU 기반
업데이터는 일반적으로 최신 CPU에서 정렬된 단일 포인터에 대한 쓰기가
원자적이라는 사실을 활용하여 판독기를 방해하지 않고 연결된 구조에서
데이터 항목을 원자적으로 삽입, 제거 및 교체할 수 있습니다. 동시
RCU 판독기는 이전 버전에 계속 액세스할 수 있으며 잠금 경합이 없는
경우에도 현재 SMP 컴퓨터 시스템에서 비용이 많이 드는 원자적 작업,
메모리 장벽 및 통신 캐시 미스를 생략할 수 있습니다.

위에 표시된 3단계 절차에서 업데이터는 제거 및 회수 단계를 모두 수행하지만
실제로 Linux 커널의 디렉터리 항목 캐시의 경우와 같이 완전히 다른 스레드가
회수를 수행하는 것이 종종 도움이 됩니다. (캐시). 동일한 스레드가 업데이트
단계(위의 (a) 단계)와 회수 단계(위의 (c) 단계)를 모두 수행하더라도 이를
별도로 생각하는 것이 도움이 되는 경우가 많습니다.
예를 들어, RCU 판독기와 업데이트 프로그램은 전혀 통신할 필요가 없지만
RCU는 판독기와 재생기 사이에 암묵적인 낮은 오버헤드 통신을 제공합니다.
즉, 위의 (b) 단계입니다.

그렇다면 판독기가 어떤 종류의 동기화 작업도 수행하지 않는다는 점을 감안할
때 판독기가 언제 판독기가 완료되었는지 알 수 있는 방법은 무엇일까요??? RCU의
API가 이를 어떻게 쉽게 만드는지 알아보려면 계속 읽어보세요.

.. _2_whatisRCU:

2.  WHAT IS RCU'S CORE API?
---------------------------

The core RCU API is quite small:

a.	rcu_read_lock()
b.	rcu_read_unlock()
c.	synchronize_rcu() / call_rcu()
d.	rcu_assign_pointer()
e.	rcu_dereference()

There are many other members of the RCU API, but the rest can be
expressed in terms of these five, though most implementations instead
express synchronize_rcu() in terms of the call_rcu() callback API.

The five core RCU APIs are described below, the other 18 will be enumerated
later.  See the kernel docbook documentation for more info, or look directly
at the function header comments.

RCU API에는 다른 많은 구성원이 있지만 대부분의 구현은 대신 call_rcu() 콜백
API 측면에서 synchronize_rcu()를 표현하지만 나머지는 이 다섯 가지 측면에서
표현할 수 있습니다.

5개의 핵심 RCU API가 아래에 설명되어 있으며 나머지 18개는 나중에 열거됩니다.
자세한 내용은 커널 설명서 문서를 참조하거나 함수 헤더 주석을 직접 살펴보십시오.

rcu_read_lock()
^^^^^^^^^^^^^^^
	void rcu_read_lock(void);

	Used by a reader to inform the reclaimer that the reader is
	entering an RCU read-side critical section.  It is illegal
	to block while in an RCU read-side critical section, though
	kernels built with CONFIG_PREEMPT_RCU can preempt RCU
	read-side critical sections.  Any RCU-protected data structure
	accessed during an RCU read-side critical section is guaranteed to
	remain unreclaimed for the full duration of that critical section.
	Reference counts may be used in conjunction with RCU to maintain
	longer-term references to data structures.

  판독기가 RCU 읽기 측 중요 섹션에 들어가고 있음을 회수자에게 알리기 
  위해 판독기가 사용합니다. CONFIG_PREEMPT_RCU로 빌드된 커널이 RCU 읽기 측 
  임계 섹션을 선점할 수 있지만 RCU 읽기 측 임계 섹션에 있는 동안 차단하는 
  것은 불법입니다. RCU 읽기 측 임계 섹션 동안 액세스된 모든 RCU 보호 데이터 
  구조는 해당 임계 섹션의 전체 기간 동안 회수되지 않은 상태로 유지됩니다.
  참조 횟수는 RCU와 함께 사용되어 데이터 구조에 대한 장기간 참조를 유지할 
  수 있습니다. 

rcu_read_unlock()
^^^^^^^^^^^^^^^^^
	void rcu_read_unlock(void);

	Used by a reader to inform the reclaimer that the reader is
	exiting an RCU read-side critical section.  Note that RCU
	read-side critical sections may be nested and/or overlapping.

  판독기가 RCU 읽기 측 중요 섹션을 종료하고 있음을 회수자에게 알리기 
  위해 판독기가 사용합니다. RCU 읽기 측 임계 섹션은 중첩 및/또는 겹칠 
  수 있습니다.

synchronize_rcu()
^^^^^^^^^^^^^^^^^
	void synchronize_rcu(void);

	Marks the end of updater code and the beginning of reclaimer
	code.  It does this by blocking until all pre-existing RCU
	read-side critical sections on all CPUs have completed.
	Note that synchronize_rcu() will **not** necessarily wait for
	any subsequent RCU read-side critical sections to complete.
	For example, consider the following sequence of events::.

  업데이트 코드의 끝과 리클레이머 코드의 시작을 표시합니다.
  모든 CPU의 모든 기존 RCU 읽기 측 중요 섹션이 완료될 때까지 
  차단하여 이를 수행합니다.
  synchronize_rcu()는 후속 RCU 읽기 측 중요 섹션이 완료될 때까지 
  반드시 대기하지 **않습니다**. 
  예를 들어 다음과 같은 일련의 이벤트를 고려하십시오.::

	         CPU 0                  CPU 1                 CPU 2
	     ----------------- ------------------------- ---------------
	 1.  rcu_read_lock()
	 2.                    enters synchronize_rcu()
	 3.                                               rcu_read_lock()
	 4.  rcu_read_unlock()
	 5.                     exits synchronize_rcu()
	 6.                                              rcu_read_unlock()

	To reiterate, synchronize_rcu() waits only for ongoing RCU
	read-side critical sections to complete, not necessarily for
	any that begin after synchronize_rcu() is invoked.

	Of course, synchronize_rcu() does not necessarily return
	**immediately** after the last pre-existing RCU read-side critical
	section completes.  For one thing, there might well be scheduling
	delays.  For another thing, many RCU implementations process
	requests in batches in order to improve efficiencies, which can
	further delay synchronize_rcu().

	Since synchronize_rcu() is the API that must figure out when
	readers are done, its implementation is key to RCU.  For RCU
	to be useful in all but the most read-intensive situations,
	synchronize_rcu()'s overhead must also be quite small.

	The call_rcu() API is a callback form of synchronize_rcu(),
	and is described in more detail in a later section.  Instead of
	blocking, it registers a function and argument which are invoked
	after all ongoing RCU read-side critical sections have completed.
	This callback variant is particularly useful in situations where
	it is illegal to block or where update-side performance is
	critically important.

	However, the call_rcu() API should not be used lightly, as use
	of the synchronize_rcu() API generally results in simpler code.
	In addition, the synchronize_rcu() API has the nice property
	of automatically limiting update rate should grace periods
	be delayed.  This property results in system resilience in face
	of denial-of-service attacks.  Code using call_rcu() should limit
	update rate in order to gain this same sort of resilience.  See
	checklist.txt for some approaches to limiting the update rate.

  다시 말해, synchronize_rcu()는 진행 중인 RCU 읽기 측 임계 섹션이 
  완료될 때까지만 대기하며, synchronize_rcu()가 호출된 후에 시작되는 
  모든 임계 섹션이 반드시 완료되는 것은 아닙니다.

  물론, synchronize_rcu()는 마지막 기존 RCU 읽기 측 임계 섹션이 
  완료된 후 반드시 **즉시** 반환되지 않습니다. 우선 일정 지연이 
  있을 수 있습니다. 또 다른 이유로, 많은 RCU 구현은 효율성을 
  향상시키기 위해 요청을 배치로 처리하며, 이로 인해 synchronize_rcu()가 
  더 지연될 수 있습니다.

  synchronize_rcu()는 리더가 언제 완료되는지 파악해야 하는 API이므로 
  구현이 RCU의 핵심입니다. 읽기 집약적인 상황을 제외한 모든 상황에서 
  RCU가 유용하려면 synchronize_rcu()의 오버헤드도 매우 작아야 합니다. 

  call_rcu() API는 synchronize_rcu()의 콜백 형태이며 이후 섹션에서 
  자세히 설명합니다. 차단하는 대신 진행 중인 모든 RCU 읽기 측 임계 
  섹션이 완료된 후에 호출되는 함수 및 인수를 등록합니다.
  이 콜백 변형은 차단하는 것이 불법이거나 업데이트 측 성능이 매우 
  중요한 상황에서 특히 유용합니다.

  그러나, 일반적으로 synchronize_rcu() API를 사용하면 코드가 단순해지기 
  때문에 call_rcu() API를 가볍게 사용해서는 안 됩니다.
  또한 synchronize_rcu() API에는 유예 기간이 지연될 경우 업데이트 속도를 
  자동으로 제한하는 좋은 속성이 있습니다. 이 속성은 서비스 거부 공격에 
  직면한 시스템 복원력을 제공합니다. call_rcu()를 사용하는 코드는 
  이와 동일한 종류의 탄력성을 얻기 위해 업데이트 속도를 제한해야 
  합니다. 업데이트 속도를 제한하는 몇 가지 방법은 checklist.txt를 
  참조하십시오.

rcu_assign_pointer()
^^^^^^^^^^^^^^^^^^^^
	void rcu_assign_pointer(p, typeof(p) v);

	Yes, rcu_assign_pointer() **is** implemented as a macro, though it
	would be cool to be able to declare a function in this manner.
	(Compiler experts will no doubt disagree.)

	The updater uses this function to assign a new value to an
	RCU-protected pointer, in order to safely communicate the change
	in value from the updater to the reader.  This macro does not
	evaluate to an rvalue, but it does execute any memory-barrier
	instructions required for a given CPU architecture.

	Perhaps just as important, it serves to document (1) which
	pointers are protected by RCU and (2) the point at which a
	given structure becomes accessible to other CPUs.  That said,
	rcu_assign_pointer() is most frequently used indirectly, via
	the _rcu list-manipulation primitives such as list_add_rcu().

  예, rcu_assign_pointer() **는** 매크로로 구현되지만 이런 방식으로 
  함수를 선언할 수 있다면 멋질 것입니다.
  (컴파일러 전문가들은 틀림없이 동의하지 않을 것입니다.).

  업데이터는 이 기능을 사용하여 RCU 보호 포인터에 새 값을 할당하여 
  업데이트 프로그램에서 판독기로 값의 변경 사항을 안전하게 전달합니다. 
  이 매크로는 rvalue로 평가되지 않지만 지정된 CPU 아키텍처에 필요한 
  메모리 장벽 명령을 실행합니다.

  아마도 그만큼 중요할 것입니다.
  (1) 어떤 포인터가 RCU에 의해 보호되는지, (2) 주어진 구조가 다른
  CPU에서 액세스할 수 있게 되는 시점을 문서화하는 역할을 합니다.
  즉, rcu_assign_pointer()는 list_add_rcu()와 같은 _rcu 목록 조작
  프리미티브를 통해 간접적으로 가장 자주 사용됩니다. 

rcu_dereference()
^^^^^^^^^^^^^^^^^
	typeof(p) rcu_dereference(p);

	Like rcu_assign_pointer(), rcu_dereference() must be implemented
	as a macro.

	The reader uses rcu_dereference() to fetch an RCU-protected
	pointer, which returns a value that may then be safely
	dereferenced.  Note that rcu_dereference() does not actually
	dereference the pointer, instead, it protects the pointer for
	later dereferencing.  It also executes any needed memory-barrier
	instructions for a given CPU architecture.  Currently, only Alpha
	needs memory barriers within rcu_dereference() -- on other CPUs,
	it compiles to nothing, not even a compiler directive.

	Common coding practice uses rcu_dereference() to copy an
	RCU-protected pointer to a local variable, then dereferences
	this local variable, for example as follows::.

  rcu_assign_pointer()와 마찬가지로 rcu_dereference()도 매크로로
  구현해야 합니다.

  판독기는 RCU 보호 포인터를 가져오기 위해 rcu_dereference()를
  사용하며, 이 포인터는 안전하게 역참조될 수 있는 값을 반환합니다.
  rcu_dereference()는 실제로 포인터를 역참조하지 않고 대신 나중에
  역참조할 수 있도록 포인터를 보호합니다. 또한 주어진 CPU 
  아키텍처에 필요한 메모리 배리어 명령을 실행합니다. 현재 Alpha만이
  rcu_dereference() 내에서 메모리 배리어를 필요로 합니다. 다른 
  CPU에서는 아무 것도 컴파일하지 않으며 컴파일러 지시문도 포함하지
  않습니다.

  일반적인 코딩 방법은 rcu_dereference()를 사용하여 RCU 보호 
  포인터를 로컬 변수에 복사한 다음 이 로컬 변수를 역참조합니다. 
  예를 들면 다음과 같습니다.::

		p = rcu_dereference(head.next);
		return p->data;

	However, in this case, one could just as easily combine these
	into one statement::.

  그러나이 경우 하나의 명령문으로 쉽게 결합 할 수 있습니다.::


		return rcu_dereference(head.next)->data;

	If you are going to be fetching multiple fields from the
	RCU-protected structure, using the local variable is of
	course preferred.  Repeated rcu_dereference() calls look
	ugly, do not guarantee that the same pointer will be returned
	if an update happened while in the critical section, and incur
	unnecessary overhead on Alpha CPUs.

	Note that the value returned by rcu_dereference() is valid
	only within the enclosing RCU read-side critical section [1]_.
	For example, the following is **not** legal::.

  RCU 보호 구조에서 여러 필드를 가져오려는 경우에는 당연히 로컬 
  변수를 사용하는 것이 좋습니다. 반복되는 rcu_dereference() 호출은 
  추해 보이고, 중요한 섹션에 있는 동안 업데이트가 발생한 경우 동일한 
  포인터가 반환될 것이라고 보장하지 않으며, Alpha CPU에 불필요한 
  오버헤드가 발생합니다.

  rcu_dereference()에 의해 반환된 값은 둘러싸는 RCU 읽기 측 임계 
  섹션 [1]_ 내에서만 유효합니다.
  예를 들어 다음은 합법적이지 **않습니다**.::

		rcu_read_lock();
		p = rcu_dereference(head.next);
		rcu_read_unlock();
		x = p->address;	/* BUG!!! */
		rcu_read_lock();
		y = p->data;	/* BUG!!! */
		rcu_read_unlock();

	Holding a reference from one RCU read-side critical section
	to another is just as illegal as holding a reference from
	one lock-based critical section to another!  Similarly,
	using a reference outside of the critical section in which
	it was acquired is just as illegal as doing so with normal
	locking.

	As with rcu_assign_pointer(), an important function of
	rcu_dereference() is to document which pointers are protected by
	RCU, in particular, flagging a pointer that is subject to changing
	at any time, including immediately after the rcu_dereference().
	And, again like rcu_assign_pointer(), rcu_dereference() is
	typically used indirectly, via the _rcu list-manipulation
	primitives, such as list_for_each_entry_rcu() [2]_.

.. 	[1] The variant rcu_dereference_protected() can be used outside
	of an RCU read-side critical section as long as the usage is
	protected by locks acquired by the update-side code.  This variant
	avoids the lockdep warning that would happen when using (for
	example) rcu_dereference() without rcu_read_lock() protection.
	Using rcu_dereference_protected() also has the advantage
	of permitting compiler optimizations that rcu_dereference()
	must prohibit.	The rcu_dereference_protected() variant takes
	a lockdep expression to indicate which locks must be acquired
	by the caller. If the indicated protection is not provided,
	a lockdep splat is emitted.  See Documentation/RCU/Design/Requirements/Requirements.rst
	and the API's code comments for more details and example usage.

.. 	[2] If the list_for_each_entry_rcu() instance might be used by
	update-side code as well as by RCU readers, then an additional
	lockdep expression can be added to its list of arguments.
	For example, given an additional "lock_is_held(&mylock)" argument,
	the RCU lockdep code would complain only if this instance was
	invoked outside of an RCU read-side critical section and without
	the protection of mylock.

The following diagram shows how each API communicates among the
reader, updater, and reclaimer.


  하나의 RCU 읽기 측 임계 섹션에서 다른 RCU로 참조를 유지하는 것은 
  하나의 잠금 기반 임계 섹션에서 다른 임계 섹션으로 참조를 유지하는 
  것만큼이나 불법입니다! 마찬가지로 획득한 임계 영역 외부에서 참조를 
  사용하는 것은 일반 잠금과 마찬가지로 불법입니다.

  rcu_assign_pointer()와 마찬가지로 rcu_dereference()의 중요한 기능은 
  어떤 포인터가 RCU에 의해 보호되는지 문서화하는 것입니다. 특히 
  rcu_dereference() 직후를 포함하여 언제든지 변경될 수 있는 포인터에 
  플래그를 지정합니다.
  그리고 다시 rcu_assign_pointer()와 마찬가지로 rcu_dereference()는 
  일반적으로 list_for_each_entry_rcu() [2]_와 같은 _rcu 목록 조작 
  프리미티브를 통해 간접적으로 사용됩니다.

..  [1] 변종 rcu_dereference_protected()는 업데이트측 코드에서 획득한 
  잠금으로 사용이 보호되는 한 RCU 읽기측 임계 섹션 외부에서 사용할 수 
  있습니다. 이 변형은 (예를 들어) rcu_read_lock() 보호 없이 
  rcu_dereference()를 사용할 때 발생할 수 있는 lockdep 경고를 방지합니다.
  rcu_dereference_protected()를 사용하면 rcu_dereference()가 금지해야 
  하는 컴파일러 최적화를 허용하는 이점도 있습니다. 
  rcu_dereference_protected() 변형은 lockdep 표현식을 사용하여 호출자가 
  획득해야 하는 잠금을 나타냅니다. 표시된 보호가 제공되지 않으면 
  lockdep splat이 방출됩니다. 자세한 내용과 사용 예는 
  Documentation/RCU/Design/Requirements/Requirements.rst 및 API의 코드 
  주석을 참조하십시오.

..  [2] list_for_each_entry_rcu() 인스턴스가 업데이트 측 코드와 RCU 
  판독기에서 사용될 수 있는 경우 추가 lockdep 표현식을 인수 목록에 
  추가할 수 있습니다.
  예를 들어 추가 "lock_is_held(&mylock)" 인수가 주어지면 RCU lockdep 
  코드는 이 인스턴스가 RCU 읽기 측 임계 섹션 외부에서 호출되고 mylock의 
  보호 없이 호출된 경우에만 불평합니다.

다음 다이어그램은 각 API가 판독기, 업데이트 프로그램 및 회수자 간에 통신하는 방법을 보여줍니다.
::


	    rcu_assign_pointer()
	                            +--------+
	    +---------------------->| reader |---------+
	    |                       +--------+         |
	    |                           |              |
	    |                           |              | Protect:
	    |                           |              | rcu_read_lock()
	    |                           |              | rcu_read_unlock()
	    |        rcu_dereference()  |              |
	    +---------+                 |              |
	    | updater |<----------------+              |
	    +---------+                                V
	    |                                    +-----------+
	    +----------------------------------->| reclaimer |
	                                         +-----------+
	      Defer:
	      synchronize_rcu() & call_rcu()


The RCU infrastructure observes the time sequence of rcu_read_lock(),
rcu_read_unlock(), synchronize_rcu(), and call_rcu() invocations in
order to determine when (1) synchronize_rcu() invocations may return
to their callers and (2) call_rcu() callbacks may be invoked.  Efficient
implementations of the RCU infrastructure make heavy use of batching in
order to amortize their overhead over many uses of the corresponding APIs.

There are at least three flavors of RCU usage in the Linux kernel. The diagram
above shows the most common one. On the updater side, the rcu_assign_pointer(),
synchronize_rcu() and call_rcu() primitives used are the same for all three
flavors. However for protection (on the reader side), the primitives used vary
depending on the flavor:

RCU 인프라는 rcu_read_lock(), rcu_read_unlock(), synchronize_rcu() 및 
call_rcu() 호출의 시간 순서를 관찰하여 (1) synchronize_rcu() 호출이 
호출자에게 반환될 수 있는 시기 및 (2) call_rcu() 콜백을 결정합니다. 호출될 
수 있습니다. RCU 인프라의 효율적인 구현은 해당 API의 많은 사용에 대한 
오버헤드를 상각하기 위해 일괄 처리를 많이 사용합니다.

Linux 커널에는 적어도 세 가지 종류의 RCU 사용이 있습니다. 위의 다이어그램은 
가장 일반적인 것을 보여줍니다. 업데이터 측에서 사용되는 rcu_assign_pointer(), 
synchronize_rcu() 및 call_rcu() 프리미티브는 세 가지 모두 동일합니다. 
그러나 (독자 측에서) 보호를 위해 사용되는 프리미티브는 취향에 따라 다릅니다.

a.	rcu_read_lock() / rcu_read_unlock()
	rcu_dereference()

b.	rcu_read_lock_bh() / rcu_read_unlock_bh()
	local_bh_disable() / local_bh_enable()
	rcu_dereference_bh()

c.	rcu_read_lock_sched() / rcu_read_unlock_sched()
	preempt_disable() / preempt_enable()
	local_irq_save() / local_irq_restore()
	hardirq enter / hardirq exit
	NMI enter / NMI exit
	rcu_dereference_sched()

These three flavors are used as follows:

a.	RCU applied to normal data structures.

b.	RCU applied to networking data structures that may be subjected
	to remote denial-of-service attacks.

c.	RCU applied to scheduler and interrupt/NMI-handler tasks.

Again, most uses will be of (a).  The (b) and (c) cases are important
for specialized uses, but are relatively uncommon.

이 세 가지 맛은 다음과 같이 사용됩니다.:

a. 일반 데이터 구조에 적용되는 RCU.

b. RCU는 원격 서비스 거부 공격을 받을 수 있는 네트워킹 데이터 구조에 
적용됩니다.

c. RCU는 스케줄러 및 인터럽트/NMI 처리기 작업에 적용됩니다.

다시 말하지만 대부분의 용도는 (a)입니다. (b) 및 (c)의 경우는 특수 
용도에 중요하지만 상대적으로 흔하지 않습니다.

.. _3_whatisRCU:

3.  WHAT ARE SOME EXAMPLE USES OF CORE RCU API?
-----------------------------------------------

This section shows a simple use of the core RCU API to protect a
global pointer to a dynamically allocated structure.  More-typical
uses of RCU may be found in :ref:`listRCU.rst <list_rcu_doc>`,
:ref:`arrayRCU.rst <array_rcu_doc>`, and :ref:`NMI-RCU.rst <NMI_rcu_doc>`.

이 섹션에서는 핵심 RCU API를 사용하여 동적으로 할당된 구조에 대한 
전역 포인터를 보호하는 방법을 보여줍니다. RCU의 보다 일반적인 
사용은 :ref:`listRCU.rst <list_rcu_doc>`, 
:ref:`arrayRCU.rst <array_rcu_doc>` 및 
:ref:`NMI-RCU.rst <NMI_rcu_doc>` 에서 찾을 수 있습니다.
::

	struct foo {
		int a;
		char b;
		long c;
	};
	DEFINE_SPINLOCK(foo_mutex);

	struct foo __rcu *gbl_foo;

	/*
	 * Create a new struct foo that is the same as the one currently
	 * pointed to by gbl_foo, except that field "a" is replaced
	 * with "new_a".  Points gbl_foo to the new structure, and
	 * frees up the old structure after a grace period.
	 *
	 * Uses rcu_assign_pointer() to ensure that concurrent readers
	 * see the initialized version of the new structure.
	 *
	 * Uses synchronize_rcu() to ensure that any readers that might
	 * have references to the old structure complete before freeing
	 * the old structure.
	 * 
	 * 필드 a가 new_a로 대체된다는 점을 제외하고 현재 gbl_foo가
	 * 가리키는 것과 동일한 새 구조체 foo를 만듭니다. gbl_foo가
	 * 새 구조를 가리키고 유예 기간 후에 이전 구조를 해제합니다.
	 * 
	 * rcu_assign_pointer()를 사용하여 동시 독자가 새 구조의 
	 * 초기화된 버전을 볼 수 있도록 합니다.
	 * 
	 * synchronize_rcu()를 사용하여 이전 구조를 해제하기 전에 이전 
	 * 구조에 대한 참조가 있을 수 있는 판독기가 완료되도록 합니다.
	 */

	void foo_update_a(int new_a)
	{
		struct foo *new_fp;
		struct foo *old_fp;

		new_fp = kmalloc(sizeof(*new_fp), GFP_KERNEL);
		spin_lock(&foo_mutex);
		old_fp = rcu_dereference_protected(gbl_foo, lockdep_is_held(&foo_mutex));
		*new_fp = *old_fp;
		new_fp->a = new_a;
		rcu_assign_pointer(gbl_foo, new_fp);
		spin_unlock(&foo_mutex);
		synchronize_rcu();
		kfree(old_fp);
	}

	/*
	 * Return the value of field "a" of the current gbl_foo
	 * structure.  Use rcu_read_lock() and rcu_read_unlock()
	 * to ensure that the structure does not get deleted out
	 * from under us, and use rcu_dereference() to ensure that
	 * we see the initialized version of the structure (important
	 * for DEC Alpha and for people reading the code).
	 *   
	 * 현재 gbl_foo 구조의 필드 a 값을 반환합니다.
	 * rcu_read_lock() 및 rcu_read_unlock()을 사용하여 구조가 우리
	 * 아래에서 삭제되지 않도록 하고 rcu_dereference()를 사용하여 구조의
	 * 초기화된 버전을 볼 수 있도록 합니다(DEC Alpha 및 코드를
	 * 읽는 사람들에게 중요).
	 */
	int foo_get_a(void)
	{
		int retval;

		rcu_read_lock();
		retval = rcu_dereference(gbl_foo)->a;
		rcu_read_unlock();
		return retval;
	}

So, to sum up:

-	Use rcu_read_lock() and rcu_read_unlock() to guard RCU
	read-side critical sections.

-	Within an RCU read-side critical section, use rcu_dereference()
	to dereference RCU-protected pointers.

-	Use some solid scheme (such as locks or semaphores) to
	keep concurrent updates from interfering with each other.

-	Use rcu_assign_pointer() to update an RCU-protected pointer.
	This primitive protects concurrent readers from the updater,
	**not** concurrent updates from each other!  You therefore still
	need to use locking (or something similar) to keep concurrent
	rcu_assign_pointer() primitives from interfering with each other.

-	Use synchronize_rcu() **after** removing a data element from an
	RCU-protected data structure, but **before** reclaiming/freeing
	the data element, in order to wait for the completion of all
	RCU read-side critical sections that might be referencing that
	data item.

See checklist.txt for additional rules to follow when using RCU.
And again, more-typical uses of RCU may be found in :ref:`listRCU.rst
<list_rcu_doc>`, :ref:`arrayRCU.rst <array_rcu_doc>`, and :ref:`NMI-RCU.rst
<NMI_rcu_doc>`.

요약하자면:.

- rcu_read_lock() 및 rcu_read_unlock()을 사용하여 RCU 읽기 측 임계 
  섹션을 보호합니다.

- RCU 읽기 측 중요 섹션 내에서 rcu_dereference()를 사용하여 RCU 보호 
  포인터를 역참조합니다.

- 동시 업데이트가 서로 간섭하지 않도록 몇 가지 견고한 체계(예: 잠금 
  또는 세마포어)를 사용합니다.

- rcu_assign_pointer()를 사용하여 RCU 보호 포인터를 업데이트합니다.
  이 프리미티브는 서로의 동시 업데이트가 **아닌** 업데이터로부터 
  동시 독자를 보호합니다! 따라서 동시 rcu_assign_pointer() 프리미티브가 
  서로 간섭하지 않도록 하려면 여전히 잠금(또는 이와 유사한 것)을 
  사용해야 합니다.

- RCU 보호 데이터 구조에서 데이터 요소를 제거 **후**하지만 데이터 
  요소를 회수/해제 **하기 전에** 모든 RCU 읽기 측 중요 섹션의 완료를 
  기다리기 위해 synchronize_rcu()를 사용합니다. 해당 데이터 항목을 
  참조할 수 있습니다.

RCU를 사용할 때 따라야 할 추가 규칙은 checklist.txt를 참조하세요.
그리고 다시, RCU의 보다 일반적인 용도는 :ref:`listRCU.rst <list_rcu_doc>`, 
:ref:`arrayRCU.rst <array_rcu_doc>` 및 :ref:`NMI-RCU.rst 
<NMI_rcu_doc>` 에서 찾을 수 있습니다.

.. _4_whatisRCU:

4.  WHAT IF MY UPDATING THREAD CANNOT BLOCK?
--------------------------------------------

In the example above, foo_update_a() blocks until a grace period elapses.
This is quite simple, but in some cases one cannot afford to wait so
long -- there might be other high-priority work to be done.

In such cases, one uses call_rcu() rather than synchronize_rcu().
The call_rcu() API is as follows::

위의 예에서 foo_update_a()는 유예 기간이 경과할 때까지 차단됩니다.
이것은 매우 간단하지만 어떤 경우에는 그렇게 오래 기다릴 여유가 없습니다. 
다른 우선 순위가 높은 작업이 수행될 수 있습니다.

이러한 경우에는 synchronize_rcu() 대신 call_rcu()를 사용합니다.
call_rcu() API는 다음과 같습니다::

	void call_rcu(struct rcu_head *head, rcu_callback_t func);

This function invokes func(head) after a grace period has elapsed.
This invocation might happen from either softirq or process context,
so the function is not permitted to block.  The foo struct needs to
have an rcu_head structure added, perhaps as follows::

이 함수는 유예 기간이 경과한 후 func(head)를 호출합니다.
이 호출은 softirq 또는 프로세스 컨텍스트에서 발생할 수 있으므로 함수 
차단이 허용되지 않습니다. foo 구조체에는 아마도 다음과 같이 추가된 
rcu_head 구조체가 있어야 합니다::

	struct foo {
		int a;
		char b;
		long c;
		struct rcu_head rcu;
	};

The foo_update_a() function might then be written as follows::

foo_update_a() 함수는 다음과 같이 작성할 수 있습니다.::
	/*
	 * Create a new struct foo that is the same as the one currently
	 * pointed to by gbl_foo, except that field "a" is replaced
	 * with "new_a".  Points gbl_foo to the new structure, and
	 * frees up the old structure after a grace period.
	 *
	 * Uses rcu_assign_pointer() to ensure that concurrent readers
	 * see the initialized version of the new structure.
	 *
	 * Uses call_rcu() to ensure that any readers that might have
	 * references to the old structure complete before freeing the
	 * old structure.
	 */
	 /*
	  * 필드 a가 new_a로 대체된다는 점을 제외하고 현재 gbl_foo가 가리키는 
	  * 것과 동일한 새 구조체 foo를 만듭니다. gbl_foo가 새 구조를 가리키고 
	  * 유예 기간 후에 이전 구조를 해제합니다.
	  *
	  * rcu_assign_pointer()를 사용하여 동시 독자가 새 구조의 초기화된 
	  * 버전을 볼 수 있도록 합니다.
	  *
	  * call_rcu()를 사용하여 이전 구조를 해제하기 전에 이전 구조에
	  * 대한 참조가 있을 수 있는 판독기가 완료되도록 합니다.
	  */
	void foo_update_a(int new_a)
	{
		struct foo *new_fp;
		struct foo *old_fp;

		new_fp = kmalloc(sizeof(*new_fp), GFP_KERNEL);
		spin_lock(&foo_mutex);
		old_fp = rcu_dereference_protected(gbl_foo, lockdep_is_held(&foo_mutex));
		*new_fp = *old_fp;
		new_fp->a = new_a;
		rcu_assign_pointer(gbl_foo, new_fp);
		spin_unlock(&foo_mutex);
		call_rcu(&old_fp->rcu, foo_reclaim);
	}

The foo_reclaim() function might appear as follows::

foo_reclaim() 함수는 다음과 같이 나타날 수 있습니다.::

	void foo_reclaim(struct rcu_head *rp)
	{
		struct foo *fp = container_of(rp, struct foo, rcu);

		foo_cleanup(fp->a);

		kfree(fp);
	}

The container_of() primitive is a macro that, given a pointer into a
struct, the type of the struct, and the pointed-to field within the
struct, returns a pointer to the beginning of the struct.

The use of call_rcu() permits the caller of foo_update_a() to
immediately regain control, without needing to worry further about the
old version of the newly updated element.  It also clearly shows the
RCU distinction between updater, namely foo_update_a(), and reclaimer,
namely foo_reclaim().

The summary of advice is the same as for the previous section, except
that we are now using call_rcu() rather than synchronize_rcu():

-	Use call_rcu() **after** removing a data element from an
	RCU-protected data structure in order to register a callback
	function that will be invoked after the completion of all RCU
	read-side critical sections that might be referencing that
	data item.

If the callback for call_rcu() is not doing anything more than calling
kfree() on the structure, you can use kfree_rcu() instead of call_rcu()
to avoid having to write your own callback::

container_of() 프리미티브는 구조체에 대한 포인터, 구조체 유형 및 
구조체 내에서 가리키는 필드가 주어지면 구조체 시작 부분에 대한 
포인터를 반환하는 매크로입니다.

call_rcu()를 사용하면 foo_update_a() 호출자가 새로 업데이트된 요소의 
이전 버전에 대해 더 이상 걱정할 필요 없이 즉시 제어권을 다시 얻을 
수 있습니다. 또한 업데이트 프로그램(foo_update_a())과 
리클레이머(foo_reclaim()) 간의 RCU 차이를 명확하게 보여줍니다.

조언 요약은 우리가 이제 synchronize_rcu() 대신 call_rcu()를 
사용하고 있다는 점을 제외하면 이전 섹션과 동일합니다.

- 해당 데이터 항목을 참조할 수 있는 모든 RCU 읽기 측 임계 섹션이 
  완료된 후 호출될 콜백 함수를 등록하기 위해 RCU 보호 데이터 구조에서 
  데이터 요소를 제거한 **후** call_rcu()를 사용합니다.

call_rcu()에 대한 콜백이 구조체에서 kfree()를 호출하는 것 이상을 
수행하지 않는 경우 call_rcu() 대신 kfree_rcu()를 사용하여 자신의 
callback::을 작성하지 않아도 됩니다.::

	kfree_rcu(old_fp, rcu);

Again, see checklist.txt for additional rules governing the use of RCU.

RCU 사용을 관리하는 추가 규칙에 대해서는 checklist.txt를 다시 참조하십시오.

.. _5_whatisRCU:

5.  WHAT ARE SOME SIMPLE IMPLEMENTATIONS OF RCU?
------------------------------------------------

One of the nice things about RCU is that it has extremely simple "toy"
implementations that are a good first step towards understanding the
production-quality implementations in the Linux kernel.  This section
presents two such "toy" implementations of RCU, one that is implemented
in terms of familiar locking primitives, and another that more closely
resembles "classic" RCU.  Both are way too simple for real-world use,
lacking both functionality and performance.  However, they are useful
in getting a feel for how RCU works.  See kernel/rcu/update.c for a
production-quality implementation, and see:

RCU의 좋은 점 중 하나는 Linux 커널에서 프로덕션 품질 구현을 이해하기 
위한 좋은 첫 단계인 매우 간단한 장난감 구현이 있다는 것입니다. 
이 섹션에서는 두 가지 RCU의 장난감 구현을 제시합니다. 하나는 친숙한 
잠금 프리미티브 측면에서 구현되고 다른 하나는 고전적인 RCU와 
더 유사합니다. 둘 다 실제 사용하기에는 너무 단순하고 기능과 성능이 
모두 부족합니다. 그러나 RCU가 작동하는 방식을 이해하는 데 유용합니다. 
프로덕션 품질 구현에 대해서는 kernel/rcu/update.c를 참조하고 다음을 참조하십시오.

	http://www.rdrop.com/users/paulmck/RCU

for papers describing the Linux kernel RCU implementation.  The OLS'01
and OLS'02 papers are a good introduction, and the dissertation provides
more details on the current implementation as of early 2004.

Linux 커널 RCU 구현을 설명하는 문서. OLS'01 및 OLS'02 논문은 좋은 
소개이며 논문은 2004년 초 현재 구현에 대한 자세한 내용을 제공합니다.

5A.  "TOY" IMPLEMENTATION #1: LOCKING
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
This section presents a "toy" RCU implementation that is based on
familiar locking primitives.  Its overhead makes it a non-starter for
real-life use, as does its lack of scalability.  It is also unsuitable
for realtime use, since it allows scheduling latency to "bleed" from
one read-side critical section to another.  It also assumes recursive
reader-writer locks:  If you try this with non-recursive locks, and
you allow nested rcu_read_lock() calls, you can deadlock.

However, it is probably the easiest implementation to relate to, so is
a good starting point.

It is extremely simple::

이 섹션에서는 친숙한 잠금 프리미티브를 기반으로 하는 장난감 RCU 구현을 
제시합니다. 확장성 부족과 마찬가지로 오버헤드로 인해 실생활에서 사용할 수 
없습니다. 또한 예약 대기 시간이 하나의 읽기 측 중요 섹션에서 다른 읽기 측 
중요 섹션으로 번질 수 있기 때문에 실시간 사용에 적합하지 않습니다. 또한 
재귀적 리더-작성기 잠금을 가정합니다. 비재귀적 잠금으로 이것을 시도하고 
중첩된 rcu_read_lock() 호출을 허용하면 교착 상태가 발생할 수 있습니다.

그러나 가장 관련성이 높은 구현이므로 좋은 출발점이 될 수 있습니다.

매우 간단합니다::

	static DEFINE_RWLOCK(rcu_gp_mutex);

	void rcu_read_lock(void)
	{
		read_lock(&rcu_gp_mutex);
	}

	void rcu_read_unlock(void)
	{
		read_unlock(&rcu_gp_mutex);
	}

	void synchronize_rcu(void)
	{
		write_lock(&rcu_gp_mutex);
		smp_mb__after_spinlock();
		write_unlock(&rcu_gp_mutex);
	}

[You can ignore rcu_assign_pointer() and rcu_dereference() without missing
much.  But here are simplified versions anyway.  And whatever you do,
don't forget about them when submitting patches making use of RCU!]::.

[rcu_assign_pointer() 및 rcu_dereference()를 많이 누락하지 않고 무시할 수 
있습니다. 그러나 어쨌든 단순화 된 버전이 있습니다. 그리고 무엇을 하든 
RCU를 사용하는 패치를 제출할 때 잊지 마세요!]::

	#define rcu_assign_pointer(p, v) \
	({ \
		smp_store_release(&(p), (v)); \
	})

	#define rcu_dereference(p) \
	({ \
		typeof(p) _________p1 = READ_ONCE(p); \
		(_________p1); \
	})


The rcu_read_lock() and rcu_read_unlock() primitive read-acquire
and release a global reader-writer lock.  The synchronize_rcu()
primitive write-acquires this same lock, then releases it.  This means
that once synchronize_rcu() exits, all RCU read-side critical sections
that were in progress before synchronize_rcu() was called are guaranteed
to have completed -- there is no way that synchronize_rcu() would have
been able to write-acquire the lock otherwise.  The smp_mb__after_spinlock()
promotes synchronize_rcu() to a full memory barrier in compliance with
the "Memory-Barrier Guarantees" listed in:

rcu_read_lock() 및 rcu_read_unlock() 프리미티브 읽기는 전역 판독기-작성기 
잠금을 획득하고 해제합니다. synchronize_rcu() 프리미티브는 이 동일한 잠금을 
쓰기 획득한 다음 해제합니다. 즉, synchronize_rcu()가 종료되면 
synchronize_rcu()가 호출되기 전에 진행 중이던 모든 RCU 읽기 측 중요 
섹션이 완료되었음을 보장합니다. 그렇지 않으면 잠급니다. 
smp_mb__after_spinlock()은 synchronize_rcu()를 다음에 나열된 메모리 
장벽 보장에 따라 전체 메모리 장벽으로 승격합니다.:

	Documentation/RCU/Design/Requirements/Requirements.rst

It is possible to nest rcu_read_lock(), since reader-writer locks may
be recursively acquired.  Note also that rcu_read_lock() is immune
from deadlock (an important property of RCU).  The reason for this is
that the only thing that can block rcu_read_lock() is a synchronize_rcu().
But synchronize_rcu() does not acquire any locks while holding rcu_gp_mutex,
so there can be no deadlock cycle.

rcu_read_lock()을 중첩하는 것이 가능합니다. 판독기-작성기 잠금이 재귀적으로 
획득될 수 있기 때문입니다. 또한 rcu_read_lock()은 교착 상태(RCU의 중요한 속성)에 
영향을 받지 않습니다. 그 이유는 rcu_read_lock()을 차단할 수 있는 것이 
synchronize_rcu()뿐이기 때문입니다.
그러나 synchronize_rcu()는 rcu_gp_mutex를 유지하는 동안 어떠한 잠금도 획득하지 
않으므로 교착 상태 주기가 있을 수 없습니다.

.. _quiz_1:

Quick Quiz #1:
		Why is this argument naive?  How could a deadlock
		occur when using this algorithm in a real-world Linux
		kernel?  How could this deadlock be avoided?

    이 주장이 순진한 이유는 무엇입니까? 실제 Linux 커널에서 이 알고리즘을 사용할 때 
    어떻게 교착 상태가 발생할 수 있습니까? 이 교착 상태를 어떻게 피할 수 있습니까?.

:ref:`Answers to Quick Quiz <8_whatisRCU>`

5B.  "TOY" EXAMPLE #2: CLASSIC RCU
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
This section presents a "toy" RCU implementation that is based on
"classic RCU".  It is also short on performance (but only for updates) and
on features such as hotplug CPU and the ability to run in CONFIG_PREEMPTION
kernels.  The definitions of rcu_dereference() and rcu_assign_pointer()
are the same as those shown in the preceding section, so they are omitted.
::

이 섹션에서는 기존 RCU를 기반으로 하는 장난감 RCU 구현을 제시합니다. 
또한 성능(업데이트에만 해당)이 부족하고 핫플러그 CPU 및 CONFIG_PREEMPTION 
커널에서 실행할 수 있는 기능과 같은 기능이 있습니다. rcu_dereference() 및 
rcu_assign_pointer()의 정의는 앞 절에서 설명한 것과 같으므로 생략한다.
::

	void rcu_read_lock(void) { }

	void rcu_read_unlock(void) { }

	void synchronize_rcu(void)
	{
		int cpu;

		for_each_possible_cpu(cpu)
			run_on(cpu);
	}

Note that rcu_read_lock() and rcu_read_unlock() do absolutely nothing.
This is the great strength of classic RCU in a non-preemptive kernel:
read-side overhead is precisely zero, at least on non-Alpha CPUs.
And there is absolutely no way that rcu_read_lock() can possibly
participate in a deadlock cycle!

The implementation of synchronize_rcu() simply schedules itself on each
CPU in turn.  The run_on() primitive can be implemented straightforwardly
in terms of the sched_setaffinity() primitive.  Of course, a somewhat less
"toy" implementation would restore the affinity upon completion rather
than just leaving all tasks running on the last CPU, but when I said
"toy", I meant **toy**!

So how the heck is this supposed to work???

Remember that it is illegal to block while in an RCU read-side critical
section.  Therefore, if a given CPU executes a context switch, we know
that it must have completed all preceding RCU read-side critical sections.
Once **all** CPUs have executed a context switch, then **all** preceding
RCU read-side critical sections will have completed.

So, suppose that we remove a data item from its structure and then invoke
synchronize_rcu().  Once synchronize_rcu() returns, we are guaranteed
that there are no RCU read-side critical sections holding a reference
to that data item, so we can safely reclaim it.

rcu_read_lock() 및 rcu_read_unlock()은 아무 작업도 수행하지 않습니다.
이것은 비선점형 커널에서 고전적인 RCU의 큰 장점입니다.
적어도 알파가 아닌 CPU에서는 읽기 측 오버헤드가 정확히 0입니다.
그리고 rcu_read_lock()이 교착 상태 사이클에 참여할 수 있는 방법은 전혀 없습니다!.

synchronize_rcu()의 구현은 각 CPU에서 차례로 자체적으로 예약됩니다. 
run_on() 프리미티브는 sched_setaffinity() 프리미티브 측면에서 간단하게 
구현할 수 있습니다. 물론, 약간 덜 장난감 구현은 마지막 CPU에서 실행 중인 
모든 작업을 그대로 두는 것보다 완료 시 선호도를 복원하지만 내가 장난감이라고 
말한 것은 **장난감**!을 의미했습니다.

그래서 도대체 어떻게 작동해야 합니까???.

RCU 읽기측 중요 섹션에 있는 동안 차단하는 것은 불법임을 기억하십시오. 
따라서 주어진 CPU가 컨텍스트 전환을 실행하는 경우 이전의 모든 RCU 읽기 측 
임계 섹션을 완료해야 한다는 것을 알고 있습니다.
**모든** CPU가 컨텍스트 전환을 실행하면 **모든** 선행 RCU 읽기 측 중요 
섹션이 완료됩니다.

따라서 구조에서 데이터 항목을 제거한 다음 synchronize_rcu()를 호출한다고 
가정합니다. synchronize_rcu()가 반환되면 해당 데이터 항목에 대한 참조를 
보유하고 있는 RCU 읽기 측 중요 섹션이 없으므로 안전하게 회수할 수 있습니다.

.. _quiz_2:

Quick Quiz #2:
		Give an example where Classic RCU's read-side
		overhead is **negative**.

    Classic RCU의 읽기 측 오버헤드가 **음수** 인 예를 들어보십시오. 

:ref:`Answers to Quick Quiz <8_whatisRCU>`

.. _quiz_3:

Quick Quiz #3:
		If it is illegal to block in an RCU read-side
		critical section, what the heck do you do in
		CONFIG_PREEMPT_RT, where normal spinlocks can block???

    RCU 읽기측 크리티컬 섹션에서 차단하는 것이 불법인 경우 일반 스핀록이 
    차단할 수 있는 CONFIG_PREEMPT_RT에서 도대체 무엇을 합니까???. 

:ref:`Answers to Quick Quiz <8_whatisRCU>`

.. _6_whatisRCU:

6.  ANALOGY WITH READER-WRITER LOCKING
--------------------------------------

Although RCU can be used in many different ways, a very common use of
RCU is analogous to reader-writer locking.  The following unified
diff shows how closely related RCU and reader-writer locking can be.

RCU는 다양한 방식으로 사용될 수 있지만 RCU의 매우 일반적인 용도는 
리더-라이터 잠금과 유사합니다. 다음 통합 diff는 RCU와 리더-라이터 
잠금이 얼마나 밀접하게 관련될 수 있는지 보여줍니다.
::

	@@ -5,5 +5,5 @@ struct el {
	 	int data;
	 	/* Other data fields */
	 };
	-rwlock_t listmutex;
	+spinlock_t listmutex;
	 struct el head;

	@@ -13,15 +14,15 @@
		struct list_head *lp;
		struct el *p;

	-	read_lock(&listmutex);
	-	list_for_each_entry(p, head, lp) {
	+	rcu_read_lock();
	+	list_for_each_entry_rcu(p, head, lp) {
			if (p->key == key) {
				*result = p->data;
	-			read_unlock(&listmutex);
	+			rcu_read_unlock();
				return 1;
			}
		}
	-	read_unlock(&listmutex);
	+	rcu_read_unlock();
		return 0;
	 }

	@@ -29,15 +30,16 @@
	 {
		struct el *p;

	-	write_lock(&listmutex);
	+	spin_lock(&listmutex);
		list_for_each_entry(p, head, lp) {
			if (p->key == key) {
	-			list_del(&p->list);
	-			write_unlock(&listmutex);
	+			list_del_rcu(&p->list);
	+			spin_unlock(&listmutex);
	+			synchronize_rcu();
				kfree(p);
				return 1;
			}
		}
	-	write_unlock(&listmutex);
	+	spin_unlock(&listmutex);
		return 0;
	 }

Or, for those who prefer a side-by-side listing::

또는 나란히 나열하는 것을 선호하는 사람들을 위해::

 1 struct el {                          1 struct el {
 2   struct list_head list;             2   struct list_head list;
 3   long key;                          3   long key;
 4   spinlock_t mutex;                  4   spinlock_t mutex;
 5   int data;                          5   int data;
 6   /* Other data fields */            6   /* Other data fields */
 7 };                                   7 };
 8 rwlock_t listmutex;                  8 spinlock_t listmutex;
 9 struct el head;                      9 struct el head;

::

  1 int search(long key, int *result)    1 int search(long key, int *result)
  2 {                                    2 {
  3   struct list_head *lp;              3   struct list_head *lp;
  4   struct el *p;                      4   struct el *p;
  5                                      5
  6   read_lock(&listmutex);             6   rcu_read_lock();
  7   list_for_each_entry(p, head, lp) { 7   list_for_each_entry_rcu(p, head, lp) {
  8     if (p->key == key) {             8     if (p->key == key) {
  9       *result = p->data;             9       *result = p->data;
 10       read_unlock(&listmutex);      10       rcu_read_unlock();
 11       return 1;                     11       return 1;
 12     }                               12     }
 13   }                                 13   }
 14   read_unlock(&listmutex);          14   rcu_read_unlock();
 15   return 0;                         15   return 0;
 16 }                                   16 }

::

  1 int delete(long key)                 1 int delete(long key)
  2 {                                    2 {
  3   struct el *p;                      3   struct el *p;
  4                                      4
  5   write_lock(&listmutex);            5   spin_lock(&listmutex);
  6   list_for_each_entry(p, head, lp) { 6   list_for_each_entry(p, head, lp) {
  7     if (p->key == key) {             7     if (p->key == key) {
  8       list_del(&p->list);            8       list_del_rcu(&p->list);
  9       write_unlock(&listmutex);      9       spin_unlock(&listmutex);
                                        10       synchronize_rcu();
 10       kfree(p);                     11       kfree(p);
 11       return 1;                     12       return 1;
 12     }                               13     }
 13   }                                 14   }
 14   write_unlock(&listmutex);         15   spin_unlock(&listmutex);
 15   return 0;                         16   return 0;
 16 }                                   17 }

Either way, the differences are quite small.  Read-side locking moves
to rcu_read_lock() and rcu_read_unlock, update-side locking moves from
a reader-writer lock to a simple spinlock, and a synchronize_rcu()
precedes the kfree().

However, there is one potential catch: the read-side and update-side
critical sections can now run concurrently.  In many cases, this will
not be a problem, but it is necessary to check carefully regardless.
For example, if multiple independent list updates must be seen as
a single atomic update, converting to RCU will require special care.

Also, the presence of synchronize_rcu() means that the RCU version of
delete() can now block.  If this is a problem, there is a callback-based
mechanism that never blocks, namely call_rcu() or kfree_rcu(), that can
be used in place of synchronize_rcu().

어느 쪽이든 그 차이는 아주 작습니다. 읽기 측 잠금은 rcu_read_lock() 및 
rcu_read_unlock으로 이동하고 업데이트 측 잠금은 판독기 작성기 잠금에서 
간단한 스핀 잠금으로 이동하며 synchronize_rcu()는 kfree()보다 우선합니다.

그러나 한 가지 잠재적 문제가 있습니다. 이제 읽기 측 및 업데이트 측 임계 
섹션이 동시에 실행될 수 있습니다. 대부분의 경우 문제가 되지 않으나, 
상관없이 잘 확인해야 합니다.
예를 들어 여러 개의 독립적인 목록 업데이트를 단일 원자 업데이트로 간주해야 
하는 경우 RCU로 변환하려면 특별한 주의가 필요합니다.

또한 synchronize_rcu()의 존재는 delete()의 RCU 버전이 이제 차단될 수 
있음을 의미합니다. 이것이 문제라면, 절대 차단하지 않는 콜백 기반 메커니즘, 
즉 synchronize_rcu() 대신 사용할 수 있는 call_rcu() 또는 kfree_rcu()가 있습니다.

.. _7_whatisRCU:

7.  FULL LIST OF RCU APIs
-------------------------

The RCU APIs are documented in docbook-format header comments in the
Linux-kernel source code, but it helps to have a full list of the
APIs, since there does not appear to be a way to categorize them
in docbook.  Here is the list, by category.

RCU API는 Linux 커널 소스 코드의 docbook 형식 헤더 주석에 문서화되어 있지만 
docbook에서 API를 분류할 수 있는 방법이 없는 것 같기 때문에 API의 전체 목록이 
있으면 도움이 됩니다. 다음은 범주별 목록입니다.

RCU list traversal::

	list_entry_rcu
	list_entry_lockless
	list_first_entry_rcu
	list_next_rcu
	list_for_each_entry_rcu
	list_for_each_entry_continue_rcu
	list_for_each_entry_from_rcu
	list_first_or_null_rcu
	list_next_or_null_rcu
	hlist_first_rcu
	hlist_next_rcu
	hlist_pprev_rcu
	hlist_for_each_entry_rcu
	hlist_for_each_entry_rcu_bh
	hlist_for_each_entry_from_rcu
	hlist_for_each_entry_continue_rcu
	hlist_for_each_entry_continue_rcu_bh
	hlist_nulls_first_rcu
	hlist_nulls_for_each_entry_rcu
	hlist_bl_first_rcu
	hlist_bl_for_each_entry_rcu

RCU pointer/list update::

	rcu_assign_pointer
	list_add_rcu
	list_add_tail_rcu
	list_del_rcu
	list_replace_rcu
	hlist_add_behind_rcu
	hlist_add_before_rcu
	hlist_add_head_rcu
	hlist_add_tail_rcu
	hlist_del_rcu
	hlist_del_init_rcu
	hlist_replace_rcu
	list_splice_init_rcu
	list_splice_tail_init_rcu
	hlist_nulls_del_init_rcu
	hlist_nulls_del_rcu
	hlist_nulls_add_head_rcu
	hlist_bl_add_head_rcu
	hlist_bl_del_init_rcu
	hlist_bl_del_rcu
	hlist_bl_set_first_rcu

RCU::

	Critical sections	Grace period		Barrier

	rcu_read_lock		synchronize_net		rcu_barrier
	rcu_read_unlock		synchronize_rcu
	rcu_dereference		synchronize_rcu_expedited
	rcu_read_lock_held	call_rcu
	rcu_dereference_check	kfree_rcu
	rcu_dereference_protected

bh::

	Critical sections	Grace period		Barrier

	rcu_read_lock_bh	call_rcu		rcu_barrier
	rcu_read_unlock_bh	synchronize_rcu
	[local_bh_disable]	synchronize_rcu_expedited
	[and friends]
	rcu_dereference_bh
	rcu_dereference_bh_check
	rcu_dereference_bh_protected
	rcu_read_lock_bh_held

sched::

	Critical sections	Grace period		Barrier

	rcu_read_lock_sched	call_rcu		rcu_barrier
	rcu_read_unlock_sched	synchronize_rcu
	[preempt_disable]	synchronize_rcu_expedited
	[and friends]
	rcu_read_lock_sched_notrace
	rcu_read_unlock_sched_notrace
	rcu_dereference_sched
	rcu_dereference_sched_check
	rcu_dereference_sched_protected
	rcu_read_lock_sched_held


SRCU::

	Critical sections	Grace period		Barrier

	srcu_read_lock		call_srcu		srcu_barrier
	srcu_read_unlock	synchronize_srcu
	srcu_dereference	synchronize_srcu_expedited
	srcu_dereference_check
	srcu_read_lock_held

SRCU: Initialization/cleanup::

	DEFINE_SRCU
	DEFINE_STATIC_SRCU
	init_srcu_struct
	cleanup_srcu_struct

All: lockdep-checked RCU-protected pointer access::

	rcu_access_pointer
	rcu_dereference_raw
	RCU_LOCKDEP_WARN
	rcu_sleep_check
	RCU_NONIDLE

See the comment headers in the source code (or the docbook generated
from them) for more information.

However, given that there are no fewer than four families of RCU APIs
in the Linux kernel, how do you choose which one to use?  The following
list can be helpful:

a.	Will readers need to block?  If so, you need SRCU.

b.	What about the -rt patchset?  If readers would need to block
	in an non-rt kernel, you need SRCU.  If readers would block
	in a -rt kernel, but not in a non-rt kernel, SRCU is not
	necessary.  (The -rt patchset turns spinlocks into sleeplocks,
	hence this distinction.)

c.	Do you need to treat NMI handlers, hardirq handlers,
	and code segments with preemption disabled (whether
	via preempt_disable(), local_irq_save(), local_bh_disable(),
	or some other mechanism) as if they were explicit RCU readers?
	If so, RCU-sched is the only choice that will work for you.

d.	Do you need RCU grace periods to complete even in the face
	of softirq monopolization of one or more of the CPUs?  For
	example, is your code subject to network-based denial-of-service
	attacks?  If so, you should disable softirq across your readers,
	for example, by using rcu_read_lock_bh().

e.	Is your workload too update-intensive for normal use of
	RCU, but inappropriate for other synchronization mechanisms?
	If so, consider SLAB_TYPESAFE_BY_RCU (which was originally
	named SLAB_DESTROY_BY_RCU).  But please be careful!

f.	Do you need read-side critical sections that are respected
	even though they are in the middle of the idle loop, during
	user-mode execution, or on an offlined CPU?  If so, SRCU is the
	only choice that will work for you.

g.	Otherwise, use RCU.

Of course, this all assumes that you have determined that RCU is in fact
the right tool for your job.

자세한 내용은 소스 코드(또는 소스 코드에서 생성된 문서)의 주석 헤더를 
참조하십시오.

그러나 Linux 커널에는 4개 이상의 RCU API 제품군이 있다는 점을 감안할 때 
사용할 제품군을 어떻게 선택합니까? 다음 목록이 도움이 될 수 있습니다.

a. 독자가 차단해야 합니까? 그렇다면 SRCU가 필요합니다.

b. -rt 패치 세트는 어떻습니까? 리더가 RT가 아닌 커널에서 차단해야 하는 
  경우 SRCU가 필요합니다. 판독기가 -rt 커널에서 차단되지만 비rt 커널에서는 
  차단되지 않는 경우 SRCU가 필요하지 않습니다. (-rt 패치 세트는 스핀록을 
  슬립록으로 전환하므로 이러한 구분이 가능합니다.)

c. NMI 핸들러, hardirq 핸들러 및 선점이 비활성화된 코드 
  세그먼트(preempt_disable(), local_irq_save(), local_bh_disable() 또는 
  기타 메커니즘을 통해)를 명시적 RCU 리더인 것처럼 처리해야 합니까? 
  그렇다면 RCU-sched가 귀하에게 적합한 유일한 선택입니다.

d. 하나 이상의 CPU에 대한 softirq 독점에도 불구하고 완료하려면 RCU 유예 
  기간이 필요합니까? 예를 들어 코드가 네트워크 기반 서비스 거부 공격의 
  대상입니까? 그렇다면 예를 들어 rcu_read_lock_bh()를 사용하여 판독기에서 
  softirq를 비활성화해야 합니다.

e. 워크로드가 RCU를 정상적으로 사용하기에는 너무 업데이트 집약적이지만 
  다른 동기화 메커니즘에는 적합하지 않습니까? 그렇다면 
  SLAB_TYPESAFE_BY_RCU(원래 이름은 SLAB_DESTROY_BY_RCU)를 고려하십시오. 
  하지만 조심하세요!.

f. 유휴 루프 중간에 있거나 사용자 모드 실행 중이거나 오프라인 CPU에 
  있어도 존중되는 읽기 측 임계 섹션이 필요합니까? 그렇다면 SRCU가 
  귀하에게 적합한 유일한 선택입니다.

g. 그렇지 않으면 RCU를 사용하십시오.

물론 이 모든 것은 RCU가 실제로 작업에 적합한 도구라고 판단했다고 가정합니다.

.. _8_whatisRCU:

8.  ANSWERS TO QUICK QUIZZES
----------------------------

Quick Quiz #1:
		Why is this argument naive?  How could a deadlock
		occur when using this algorithm in a real-world Linux
		kernel?  [Referring to the lock-based "toy" RCU
		algorithm.]

    이 주장이 순진한 이유는 무엇입니까? 실제 Linux 커널에서 
    이 알고리즘을 사용할 때 어떻게 교착 상태가 발생할 수 있습니까? 
    [잠금 기반 장난감 RCU 알고리즘 참조]. 

Answer:
		Consider the following sequence of events:

		1.	CPU 0 acquires some unrelated lock, call it
			"problematic_lock", disabling irq via
			spin_lock_irqsave().

		2.	CPU 1 enters synchronize_rcu(), write-acquiring
			rcu_gp_mutex.

		3.	CPU 0 enters rcu_read_lock(), but must wait
			because CPU 1 holds rcu_gp_mutex.

		4.	CPU 1 is interrupted, and the irq handler
			attempts to acquire problematic_lock.

		The system is now deadlocked.

		One way to avoid this deadlock is to use an approach like
		that of CONFIG_PREEMPT_RT, where all normal spinlocks
		become blocking locks, and all irq handlers execute in
		the context of special tasks.  In this case, in step 4
		above, the irq handler would block, allowing CPU 1 to
		release rcu_gp_mutex, avoiding the deadlock.

		Even in the absence of deadlock, this RCU implementation
		allows latency to "bleed" from readers to other
		readers through synchronize_rcu().  To see this,
		consider task A in an RCU read-side critical section
		(thus read-holding rcu_gp_mutex), task B blocked
		attempting to write-acquire rcu_gp_mutex, and
		task C blocked in rcu_read_lock() attempting to
		read_acquire rcu_gp_mutex.  Task A's RCU read-side
		latency is holding up task C, albeit indirectly via
		task B.

		Realtime RCU implementations therefore use a counter-based
		approach where tasks in RCU read-side critical sections
		cannot be blocked by tasks executing synchronize_rcu().

    다음 이벤트 순서를 고려하십시오.

    1. CPU 0은 관련 없는 일부 잠금을 획득하고 문제가 있는 잠금이라고 
      하며 spin_lock_irqsave()를 통해 irq를 비활성화합니다.

    2. CPU 1은 synchronize_rcu()에 들어가 rcu_gp_mutex 쓰기를 획득합니다.

    3. CPU 0이 rcu_read_lock()에 들어가지만 CPU 1이 rcu_gp_mutex를 보유하고 있기 때문에 기다려야 합니다.

    4. CPU 1이 인터럽트되고 irq 핸들러가 problem_lock을 획득하려고 시도합니다.

    이제 시스템이 교착 상태에 빠졌습니다.

    이 교착 상태를 피하는 한 가지 방법은 CONFIG_PREEMPT_RT와 같은 접근 
    방식을 사용하는 것입니다. 여기서 모든 일반 스핀 잠금은 차단 잠금이 
    되고 모든 irq 처리기는 특수 작업의 컨텍스트에서 실행됩니다. 
    이 경우 위의 4단계에서 irq 처리기가 차단되어 CPU 1이 교착 상태를 
    피하면서 rcu_gp_mutex를 해제할 수 있습니다. 

    교착 상태가 없는 경우에도 이 RCU 구현을 통해 synchronize_rcu()를 통해 
    판독기에서 다른 판독기로 대기 시간이 번질 수 있습니다. 이를 확인하려면 
    RCU 읽기 측 중요 섹션(따라서 읽기 보유 rcu_gp_mutex)의 작업 A, 
    rcu_gp_mutex 쓰기 획득 시도를 차단한 작업 B, rcu_gp_mutex read_획득을 
    시도하는 rcu_read_lock()에서 차단된 작업 C를 고려하십시오. 
    작업 A의 RCU 읽기 측 대기 시간은 간접적으로 작업 B를 통해 
    작업 C를 지연시키고 있습니다.

    따라서 실시간 RCU 구현은 synchronize_rcu()를 실행하는 작업에 의해 
    RCU 읽기 측 임계 섹션의 작업이 차단될 수 없는 카운터 기반 접근 
    방식을 사용합니다. 

:ref:`Back to Quick Quiz #1 <quiz_1>`

Quick Quiz #2:
		Give an example where Classic RCU's read-side
		overhead is **negative**.

    Classic RCU의 읽기 측 오버헤드가 **음수** 인 예를 들어보십시오. 

Answer:
		Imagine a single-CPU system with a non-CONFIG_PREEMPTION
		kernel where a routing table is used by process-context
		code, but can be updated by irq-context code (for example,
		by an "ICMP REDIRECT" packet).	The usual way of handling
		this would be to have the process-context code disable
		interrupts while searching the routing table.  Use of
		RCU allows such interrupt-disabling to be dispensed with.
		Thus, without RCU, you pay the cost of disabling interrupts,
		and with RCU you don't.

		One can argue that the overhead of RCU in this
		case is negative with respect to the single-CPU
		interrupt-disabling approach.  Others might argue that
		the overhead of RCU is merely zero, and that replacing
		the positive overhead of the interrupt-disabling scheme
		with the zero-overhead RCU scheme does not constitute
		negative overhead.

		In real life, of course, things are more complex.  But
		even the theoretical possibility of negative overhead for
		a synchronization primitive is a bit unexpected.  ;-)

    라우팅 테이블이 프로세스 컨텍스트 코드에 의해 사용되지만 
    irq 컨텍스트 코드(예: ICMP REDIRECT 패킷)에 의해 업데이트될 
    수 있는 non-CONFIG_PREEMPTION 커널이 있는 단일 CPU 시스템을 
    상상해 보십시오. 이를 처리하는 일반적인 방법은 라우팅 테이블을 
    검색하는 동안 프로세스 컨텍스트 코드가 인터럽트를 
    비활성화하도록 하는 것입니다. RCU를 사용하면 이러한 인터럽트 
    비활성화를 생략할 수 있습니다.
    따라서 RCU가 없으면 인터럽트 비활성화 비용을 지불하고 
    RCU를 사용하면 그렇지 않습니다.

    이 경우 RCU의 오버헤드는 단일 CPU 인터럽트 비활성화 접근 
    방식과 관련하여 음수라고 주장할 수 있습니다. 다른 사람들은 
    RCU의 오버헤드가 단지 0이고 인터럽트 비활성화 방식의 양의 
    오버헤드를 제로 오버헤드 RCU 방식으로 대체하는 것이 음의 
    오버헤드를 구성하지 않는다고 주장할 수 있습니다.

    물론 실생활에서는 상황이 더 복잡합니다. 그러나 동기화 
    프리미티브에 대한 부정적인 오버헤드의 이론적인 가능성도 
    약간 예상치 못한 것입니다. ;-). 

:ref:`Back to Quick Quiz #2 <quiz_2>`

Quick Quiz #3:
		If it is illegal to block in an RCU read-side
		critical section, what the heck do you do in
		CONFIG_PREEMPT_RT, where normal spinlocks can block???

    RCU 읽기측 크리티컬 섹션에서 차단하는 것이 불법인 경우 
    일반 스핀록이 차단할 수 있는 CONFIG_PREEMPT_RT에서 
    도대체 무엇을 합니까???. 

Answer:
		Just as CONFIG_PREEMPT_RT permits preemption of spinlock
		critical sections, it permits preemption of RCU
		read-side critical sections.  It also permits
		spinlocks blocking while in RCU read-side critical
		sections.

		Why the apparent inconsistency?  Because it is
		possible to use priority boosting to keep the RCU
		grace periods short if need be (for example, if running
		short of memory).  In contrast, if blocking waiting
		for (say) network reception, there is no way to know
		what should be boosted.  Especially given that the
		process we need to boost might well be a human being
		who just went out for a pizza or something.  And although
		a computer-operated cattle prod might arouse serious
		interest, it might also provoke serious objections.
		Besides, how does the computer know what pizza parlor
		the human being went to???

    CONFIG_PREEMPT_RT가 스핀락 크리티컬 섹션의 선점을 
    허용하는 것처럼 RCU 읽기 측 크리티컬 섹션의 선점을 
    허용합니다. 또한 RCU 읽기 측 임계 섹션에 있는 동안 스핀록 
    차단을 허용합니다.

    명백한 불일치가 나타나는 이유는 무엇입니까? 필요한 
    경우(예: 메모리가 부족한 경우) RCU 유예 기간을 짧게 
    유지하기 위해 우선 순위 부스팅을 사용할 수 있기 때문입니다. 
    반대로 네트워크 수신 대기를 차단하는 경우 무엇을 부스트해야 
    하는지 알 수 있는 방법이 없습니다. 특히 우리가 강화해야 할 
    프로세스가 방금 피자를 먹으러 나간 사람일 수도 있다는 점을 
    감안하면요. 그리고 컴퓨터로 작동하는 소몰이가 심각한 관심을 
    불러일으킬 수도 있지만 심각한 반대를 불러일으킬 수도 있습니다.
    게다가 컴퓨터는 인간이 어느 피자 가게에 갔는지 어떻게 
    압니까???. 

:ref:`Back to Quick Quiz #3 <quiz_3>`

ACKNOWLEDGEMENTS

My thanks to the people who helped make this human-readable, including
Jon Walpole, Josh Triplett, Serge Hallyn, Suzanne Wood, and Alan Stern.


For more information, see http://www.rdrop.com/users/paulmck/RCU.
