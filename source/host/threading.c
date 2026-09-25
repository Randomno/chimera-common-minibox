/* Guest green threads: cooperative, host-scheduled, one running at a time.
 * A guest pthread is created by the custom NR_WBX_CLONE syscall; a context
 * switch is just swapping Context.guest_rsp + thread_area. Faithful C port of
 * BizHawk waterboxhost src/threading.rs (single-host, phase 2). */
#include "minibox_internal.h"
#include "minibox_threads.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Set MB_TDBG=1 to trace thread ops (spawn/exit/futex) - off by default. */
static int mb_tdbg(void) { static int v = -1; if (v < 0) { const char *e = getenv("MB_TDBG"); v = e && *e ? 1 : 0; } return v; }
#define TDBG(...) do { if (mb_tdbg()) fprintf(stderr, "[T] " __VA_ARGS__); } while (0)

#define FUTEX_WAITERS 0x80000000u

static uintptr_t sok(mb_sword v) { return (uintptr_t)v; }
static uintptr_t serr(int e) { return (uintptr_t)(intptr_t)(-e); }

typedef enum { T_RUNNABLE, T_WAITING } tstate;
typedef struct {
	uint32_t tid;
	tstate state;
	uintptr_t rax;         /* return value handed to the guest when next run */
	uintptr_t rsp;         /* guest_rsp when next run */
	uintptr_t thread_area; /* pthread_self */
	uintptr_t tid_address; /* set_tid_address */
	mb_range stack;        /* this thread's own stack, from its pthread struct */
	mb_range pending;      /* a munmap of that stack, held until it has left it */
} gthread;

/* Timed-wait expiry on deadlock. Native threads use TIMED futex
 * waits (timeouts fire, callers retry) for handshakes; ignoring timeouts
 * deadlocks green threads that would otherwise make progress. There is no
 * clock in the box, so expiry is logical, not temporal: when NO thread is
 * runnable, the earliest-parked waiter that HAD a timeout is unparked with
 * ETIMEDOUT instead of declaring death. Fully deterministic (park order
 * is guest-determined); callers must tolerate it exactly like a real
 * early/spurious timeout (they re-check + retry). Infinite waiters still
 * deadlock (genuine). */
typedef struct { uintptr_t addr; uint32_t *tids; uint64_t *deadline; size_t n, cap; } futex_queue;

struct mb_threads {
	uint32_t next_tid;
	uint32_t active_tid;
	gthread *threads; size_t nthreads, cap;   /* kept sorted by tid ascending */
	futex_queue *futicies; size_t nfut, futcap;
	/* Logical CLOCK_REALTIME (ns). clock_gettime is frozen by design
	 * (deterministic replay), so timed waits can only expire against
	 * a clock that advances when they do: no-runnable expiry
	 * fast-forwards it to the earliest deadline, and nanosleep advances
	 * it by its duration. Starts at the same constant clock_gettime
	 * always returned. */
	uint64_t clock_ns;
};
#define MB_CLOCK_INIT_NS (1495889068ull * 1000000000ull)

/* ---- thread array (sorted by tid) ---- */
static gthread *find_thread(mb_threads *t, uint32_t tid) {
	for (size_t i = 0; i < t->nthreads; i++) if (t->threads[i].tid == tid) return &t->threads[i];
	return NULL;
}
static void insert_thread(mb_threads *t, gthread g) {
	if (t->nthreads == t->cap) { t->cap = t->cap ? t->cap * 2 : 8; t->threads = realloc(t->threads, t->cap * sizeof(gthread)); }
	size_t i = t->nthreads;
	while (i > 0 && t->threads[i-1].tid > g.tid) { t->threads[i] = t->threads[i-1]; i--; }
	t->threads[i] = g; t->nthreads++;
}
static void remove_thread(mb_threads *t, uint32_t tid) {
	for (size_t i = 0; i < t->nthreads; i++)
		if (t->threads[i].tid == tid) { memmove(&t->threads[i], &t->threads[i+1], (t->nthreads-i-1)*sizeof(gthread)); t->nthreads--; return; }
}

/* ---- futex queues ---- */
static futex_queue *find_queue(mb_threads *t, uintptr_t addr) {
	for (size_t i = 0; i < t->nfut; i++) if (t->futicies[i].addr == addr) return &t->futicies[i];
	return NULL;
}
static futex_queue *get_or_make_queue(mb_threads *t, uintptr_t addr) {
	futex_queue *q = find_queue(t, addr);
	if (q) return q;
	if (t->nfut == t->futcap) { t->futcap = t->futcap ? t->futcap*2 : 4; t->futicies = realloc(t->futicies, t->futcap*sizeof(futex_queue)); }
	q = &t->futicies[t->nfut++];
	q->addr = addr; q->tids = NULL; q->deadline = NULL; q->n = 0; q->cap = 0;
	return q;
}
static void queue_push(futex_queue *q, uint32_t tid, uint64_t deadline_ns) {
	if (q->n == q->cap) {
		q->cap = q->cap ? q->cap*2 : 4;
		q->tids = realloc(q->tids, q->cap*sizeof(uint32_t));
		q->deadline = realloc(q->deadline, q->cap*sizeof(uint64_t));
	}
	q->tids[q->n] = tid; q->deadline[q->n] = deadline_ns; q->n++;
}
static void queue_remove_at(futex_queue *q, size_t i) {
	memmove(&q->tids[i], &q->tids[i+1], (q->n-i-1)*sizeof(uint32_t));
	memmove(&q->deadline[i], &q->deadline[i+1], (q->n-i-1)*sizeof(uint64_t));
	q->n--;
}
static void remove_queue(mb_threads *t, uintptr_t addr) {
	for (size_t i = 0; i < t->nfut; i++)
		if (t->futicies[i].addr == addr) { free(t->futicies[i].tids); free(t->futicies[i].deadline); memmove(&t->futicies[i], &t->futicies[i+1], (t->nfut-i-1)*sizeof(futex_queue)); t->nfut--; return; }
}

/* returns tid unparked (and whether more remain), or -1 */
static int unpark_one(mb_threads *t, uintptr_t addr, uint32_t *out_tid, bool *has_more) {
	futex_queue *q = find_queue(t, addr);
	if (!q || q->n == 0) return -1;
	uint32_t tid = q->tids[0];
	queue_remove_at(q, 0);
	gthread *g = find_thread(t, tid); if (g) g->state = T_RUNNABLE;
	*out_tid = tid;
	if (q->n == 0) { remove_queue(t, addr); *has_more = false; } else *has_more = true;
	return 0;
}
/* earliest deadline anywhere (0 = none timed); victim indices for it.
 * Queues are FIFO per addr, addrs scanned in creation order: the victim
 * is guest-deterministic. */
static int unpark_earliest_deadline(mb_threads *t, uint32_t *out_tid, uint64_t *out_dl) {
	uint64_t best = 0;
	int bi = -1; size_t bj = 0;
	for (size_t i = 0; i < t->nfut; i++) {
		futex_queue *q = &t->futicies[i];
		for (size_t j = 0; j < q->n; j++)
			if (q->deadline[j] != 0 && (best == 0 || q->deadline[j] < best)) {
				best = q->deadline[j]; bi = (int)i; bj = j;
			}
	}
	if (bi < 0) return -1;
	futex_queue *q = &t->futicies[bi];
	uint32_t tid = q->tids[bj];
	queue_remove_at(q, bj);
	gthread *g = find_thread(t, tid); if (g) g->state = T_RUNNABLE;
	*out_tid = tid; *out_dl = best;
	if (q->n == 0) remove_queue(t, q->addr);
	return 0;
}
/* unpark every waiter whose deadline has passed (each gets ETIMEDOUT
 * when next scheduled); returns count. Called on every futex_wait so
 * due deadlines fire even while other threads stay runnable. */
static int expire_due(mb_threads *t) {
	int n = 0;
	for (;;) {
		/* earliest DUE deadline (<= clock) */
		uint64_t best = 0;
		int bi = -1; size_t bj = 0;
		for (size_t i = 0; i < t->nfut; i++) {
			futex_queue *q = &t->futicies[i];
			for (size_t j = 0; j < q->n; j++)
				if (q->deadline[j] != 0 && q->deadline[j] <= t->clock_ns &&
				    (best == 0 || q->deadline[j] < best)) {
					best = q->deadline[j]; bi = (int)i; bj = j;
				}
		}
		if (bi < 0) return n;
		futex_queue *q = &t->futicies[bi];
		uint32_t victim = q->tids[bj];
		queue_remove_at(q, bj);
		gthread *g = find_thread(t, victim);
		if (g) { g->state = T_RUNNABLE; g->rax = serr(ETIMEDOUT); }
		if (q->n == 0) remove_queue(t, q->addr);
		TDBG("futex_expire_due tid=%u\n", victim);
		n++;
	}
}
/* ---- lifecycle ---- */
mb_threads *mb_threads_new(void) {
	mb_threads *t = calloc(1, sizeof(mb_threads));
	t->next_tid = 2;
	t->active_tid = 1;
	t->clock_ns = MB_CLOCK_INIT_NS;
	gthread main_thread = { 1, T_RUNNABLE, sok(0), 0, 0, 0 };
	insert_thread(t, main_thread);
	return t;
}

/* logical clock accessors (host.c: clock_gettime/nanosleep). */
uint64_t mb_threads_clock_get(mb_threads *t) { return t ? t->clock_ns : MB_CLOCK_INIT_NS; }
void mb_threads_clock_advance(mb_threads *t, uint64_t delta_ns) { if (t) t->clock_ns += delta_ns; }
uint32_t mb_threads_active_tid(mb_threads *t) { return t ? t->active_tid : 0; }
/* time-read tick: observation advances virtual time (a spinning try
 * consumes time, like native), then due deadlines expire. Deterministic
 * (observation-counted). Lets deadline-bounded pause-spins converge. */
void mb_threads_tick(mb_threads *t) {
	if (!t) return;
	/* Observation advances virtual time (a spinning try consumes time,
	 * like native), then due deadlines expire. Deterministic
	 * (observation-counted). MB_TICK_NS overrides the step (default 1us:
	 * 1ms steps caused false futex timeouts — a microsecond main-thread
	 * spin consumed milliseconds — while 0 froze deadline-spins; 1us
	 * lets both converge (verified against deadline-bounded pause-spins). */
	static long long tick_ns = -1;
	if (tick_ns < 0) {
		const char *e = getenv("MB_TICK_NS");
		tick_ns = e ? atoll(e) : 1000ll;
	}
	t->clock_ns += (uint64_t)tick_ns;
	expire_due(t);
}
void mb_threads_free(mb_threads *t) {
	if (!t) return;
	for (size_t i = 0; i < t->nfut; i++) { free(t->futicies[i].tids); free(t->futicies[i].deadline); }
	free(t->futicies); free(t->threads); free(t);
}

/* ---- scheduling ---- */
static uintptr_t swap_to(mb_threads *t, mb_context *c, uint32_t tid, uintptr_t ret) {
	gthread *old = find_thread(t, t->active_tid);
	old->rax = ret; old->rsp = c->guest_rsp; old->thread_area = c->thread_area;
	gthread *nw = find_thread(t, tid);
	c->guest_rsp = nw->rsp; c->thread_area = nw->thread_area;
	t->active_tid = tid;
	return nw->rax;
}

/* next runnable tid in ascending order wrapping around active */
static uintptr_t swap_to_next(mb_threads *t, mb_context *c, uintptr_t ret) {
	uint32_t best = 0; bool found = false;
	/* first strictly-greater tid, then wrap to smallest */
	for (size_t i = 0; i < t->nthreads; i++)
		if (t->threads[i].tid > t->active_tid && t->threads[i].state == T_RUNNABLE) { best = t->threads[i].tid; found = true; break; }
	if (!found)
		for (size_t i = 0; i < t->nthreads; i++)
			if (t->threads[i].state == T_RUNNABLE) { best = t->threads[i].tid; found = true; break; }
	if (!found) {
		/* no runnable thread: fast-forward the logical clock to the
		 * earliest timed-wait deadline and expire that waiter with
		 * ETIMEDOUT (see futex_queue note). Genuine all-infinite
		 * deadlock still stops the machine. */
		uint32_t victim = 0; uint64_t dl = 0;
		if (unpark_earliest_deadline(t, &victim, &dl) == 0) {
			if (dl > t->clock_ns) t->clock_ns = dl;
			TDBG("futex_expire tid=%u clock=%llu\n", victim, (unsigned long long)t->clock_ns);
			return swap_to(t, c, victim, serr(ETIMEDOUT));
		}
		/* every thread waits on another and none is left to wake them: a
		 * deadlock, which on real hardware is a hang - here it stops the machine */
		char states[160]; size_t o = 0;
		for (size_t i = 0; i < t->nthreads && o + 16 < sizeof states; i++)
			o += (size_t)snprintf(states + o, sizeof states - o, " t%u=%s", t->threads[i].tid,
			                      t->threads[i].state == T_RUNNABLE ? "R" : "W");
		states[o] = '\0';
		mb_host_guest_death(c, "every thread of the core is waiting and none can wake the others (a deadlock:%s)", states);
	}
	if (best == t->active_tid) return ret;   /* yield that didn't change thread */
	return swap_to(t, c, best, ret);
}

static uintptr_t park_me(mb_threads *t, mb_context *c, uintptr_t ret, uintptr_t addr, uint64_t deadline_ns) {
	queue_push(get_or_make_queue(t, addr), t->active_tid, deadline_ns);
	find_thread(t, t->active_tid)->state = T_WAITING;
	/* Wait-site backtrace (host VA == guest VA). */
	if (mb_tdbg()) {
		uint64_t *sp = (uint64_t *)c->guest_rsp;
		fprintf(stderr, "[T] park tid=%u rsp=%lx stack:", t->active_tid, (unsigned long)c->guest_rsp);
		for (int i = 0; i < 64; i++) fprintf(stderr, " %lx", (unsigned long)sp[i]);
		fprintf(stderr, "\n");
	}
	return swap_to_next(t, c, ret);
}
static void park_other(mb_threads *t, uintptr_t addr, uint32_t tid) {
	queue_push(get_or_make_queue(t, addr), tid, 0);
	find_thread(t, tid)->state = T_WAITING;
}

/* ---- syscalls ---- */
mb_sword mb_threads_spawn(mb_threads *t, mb_block *b, uintptr_t thread_area,
                      uintptr_t guest_rsp, uintptr_t guest_rip, uintptr_t child_tid, uint32_t *parent_tid) {
	uint32_t tid = t->next_tid;
	/* thread_area carries the musl pthread struct (words 12,13 are
	 * stack_end/size). A NULL area is a foreign convention (musl's own
	 * __clone, which speaks raw clone, not wbx_clone): refuse with EINVAL
	 * instead of faulting the host on pthread[12]. */
	if (thread_area == 0) return -EINVAL;
	const uintptr_t *pthread = (const uintptr_t *)thread_area;
	uintptr_t stack_end = pthread[12], stack_size = pthread[13];
	mb_range stack = { stack_end - stack_size, stack_size };
	int rc = mb_block_mprotect(b, mb_range_align_expand(stack), MB_PROT_RWSTACK);
	if (rc != 0) return rc;
	/* set up the child's initial frame so guest_syscall's `pop rbp; ret` lands at guest_rip */
	uintptr_t *child_stack = (uintptr_t *)(guest_rsp - 16);
	child_stack[0] = 0;          /* rbp */
	child_stack[1] = guest_rip;  /* ret target */
	*parent_tid = tid;
	gthread g = { tid, T_RUNNABLE, sok(0), guest_rsp - 16, thread_area, child_tid, stack, { 0, 0 } };
	insert_thread(t, g);
	t->next_tid++;
	TDBG("spawn tid=%u rip=%lx rsp=%lx\n",tid,(unsigned long)guest_rip,(unsigned long)(guest_rsp-16));
	return (mb_sword)tid;
}

uintptr_t mb_threads_exit(mb_threads *t, mb_context *c) {
	if (t->active_tid == 1) mb_host_guest_death(c, "the core's main thread exited");
	gthread *self = find_thread(t, t->active_tid);
	uintptr_t addr = self->tid_address;
	if (addr != 0) { *(uint32_t *)addr = 0; uint32_t tid; bool more; unpark_one(t, addr, &tid, &more); }
	TDBG("exit tid=%u\n",t->active_tid);
	uint32_t dead = t->active_tid;
	uintptr_t ret = swap_to_next(t, c, sok(0));
	if (t->active_tid == dead) mb_host_guest_death(c, "the core's last thread exited");
	remove_thread(t, dead);
	return ret;
}

/* A thread that is ending unmaps its OWN stack and then exits - musl's
 * __unmapself, which on real Linux is asm that touches no stack between the two
 * syscalls. Here a syscall is a call OUT of the box that has to RETURN, onto
 * exactly the stack just freed. So hold the unmap: take it at exit, once the
 * thread has been swapped off that stack for good. Without this the return path
 * reads a freed page and the box takes the fault.
 * Returns true when the range was taken over and must not be unmapped now. */
bool mb_threads_hold_stack_unmap(mb_threads *t, mb_range r) {
	gthread *self = find_thread(t, t->active_tid);
	if (self == NULL || self->stack.size == 0) return false;
	const uintptr_t rs = r.start, re = r.start + r.size;
	const uintptr_t ss = self->stack.start, se = self->stack.start + self->stack.size;
	if (rs >= se || re <= ss) return false;   /* nothing to do with this thread's stack */
	self->pending = r;
	TDBG("hold stack unmap tid=%u addr=%lx size=%lx\n",
		t->active_tid,(unsigned long)r.start,(unsigned long)r.size);
	return true;
}

/* The unmap held above, to be done by the caller once mb_threads_exit returns
 * - by then the dead thread is off its stack and another one is running. */
bool mb_threads_take_held_unmap(mb_threads *t, mb_range *out) {
	gthread *self = find_thread(t, t->active_tid);
	if (self == NULL || self->pending.size == 0) return false;
	*out = self->pending;
	self->pending = (mb_range){ 0, 0 };
	return true;
}

uintptr_t mb_threads_futex_wait(mb_threads *t, mb_context *c, uintptr_t addr, uint32_t compare, uint64_t deadline_ns) {
	expire_due(t); /* due deadlines fire even while others stay runnable */
	uint32_t cur=*(uint32_t*)addr; TDBG("futex_wait tid=%u addr=%lx cur=%u cmp=%u dl=%llu\n",t->active_tid,(unsigned long)addr,cur,compare,(unsigned long long)deadline_ns);
	if (cur != compare) return serr(EAGAIN);
	if (deadline_ns != 0 && deadline_ns <= t->clock_ns) {
		/* Already due: spin forward (a spinning try consumes virtual
		 * time, like native) and yield so expired others can run; the
		 * try itself reports expiry (spurious-safe: callers re-check). */
		t->clock_ns += 1000000ull; /* 1ms per instant expiry */
		return swap_to_next(t, c, serr(ETIMEDOUT));
	}
	return park_me(t, c, sok(0), addr, deadline_ns);
}

mb_sword mb_threads_futex_requeue(mb_threads *t, uintptr_t from, uintptr_t to, uint32_t wake, uint32_t requeue) {
	long count = 0;
	while (wake > 0 || requeue > 0) {
		uint32_t tid; bool more;
		if (unpark_one(t, from, &tid, &more) != 0) break;
		count++;
		if (wake > 0) wake--;
		else { park_other(t, to, tid); requeue--; }
		if (!more) break;
	}
	return count;
}
mb_sword mb_threads_futex_wake(mb_threads *t, mb_context *c, uintptr_t addr, uint32_t count) {
	TDBG("futex_wake tid=%u addr=%lx count=%u\n",t->active_tid,(unsigned long)addr,count);
	mb_sword n = mb_threads_futex_requeue(t, addr, 0, count, 0);
	/* Yield-after-wake: a woken thread is runnable but never runs until
	 * someone yields/parks; a waker spinning without syscalls (pause
	 * loop) would starve it (native preemption covers this). Yielding
	 * here hands off deterministically (round-robin); a no-op when
	 * nobody else is runnable. */
	if (n > 0) return swap_to_next(t, c, sok((uintptr_t)n));
	return n;
}

uintptr_t mb_threads_futex_lock_pi(mb_threads *t, mb_context *c, uintptr_t addr) {
	uint32_t *atom = (uint32_t *)addr;
	if (*atom == 0) { *atom = t->active_tid; return sok(0); }
	*atom |= FUTEX_WAITERS;
	return park_me(t, c, sok(0), addr, 0);
}
uintptr_t mb_threads_futex_unlock_pi(mb_threads *t, mb_context *c, uintptr_t addr) {
	uint32_t *atom = (uint32_t *)addr;
	uint32_t tid; bool more;
	if (unpark_one(t, addr, &tid, &more) == 0) {
		*atom = more ? (tid | FUTEX_WAITERS) : tid;
		return swap_to(t, c, tid, sok(0));   /* fair handoff */
	}
	*atom = 0;
	return sok(0);
}

uint32_t mb_threads_set_tid_address(mb_threads *t, uintptr_t addr) {
	gthread *g = find_thread(t, t->active_tid);
	g->tid_address = addr;
	return g->tid;
}
uint32_t mb_threads_get_tid(mb_threads *t) { return t->active_tid; }
bool mb_threads_has_thread(mb_threads *t, uint32_t tid) { return find_thread(t, tid) != NULL; }
void mb_threads_reset_active(mb_threads *t) { t->active_tid = 1; }
uintptr_t mb_threads_yield(mb_threads *t, mb_context *c) { return swap_to_next(t, c, sok(0)); }

/* ---- savestate (self-consistent; encoding is implementation-defined per SPEC) ---- */
static int wr(mb_write_cb w, uintptr_t ud, const void *d, size_t n) { return w(ud, d, n) < 0 ? -1 : 0; }
static int rd(mb_read_cb r, uintptr_t ud, void *d, size_t n) { uint8_t *p = d; while (n) { intptr_t g = r(ud, p, n); if (g <= 0) return -1; p += g; n -= (size_t)g; } return 0; }

int mb_threads_save(mb_threads *t, mb_context *c, mb_write_cb w, uintptr_t ud) {
	if (t->active_tid != 1) { fprintf(stderr, "miniBox: thread hijack on save\n"); return -1; }
	gthread *main = find_thread(t, 1);
	main->thread_area = c->thread_area; main->rsp = c->guest_rsp;
	if (wr(w, ud, "GuestThreadSet", 14)) return -1;
	if (wr(w, ud, &t->next_tid, 4) || wr(w, ud, &t->active_tid, 4)) return -1;
	uint32_t nt = (uint32_t)t->nthreads;
	if (wr(w, ud, &nt, 4)) return -1;
	for (size_t i = 0; i < t->nthreads; i++) if (wr(w, ud, &t->threads[i], sizeof(gthread))) return -1;
	uint32_t nf = (uint32_t)t->nfut;
	if (wr(w, ud, &nf, 4)) return -1;
	for (size_t i = 0; i < t->nfut; i++) {
		if (wr(w, ud, &t->futicies[i].addr, sizeof(uintptr_t))) return -1;
		uint32_t qn = (uint32_t)t->futicies[i].n;
		if (wr(w, ud, &qn, 4)) return -1;
		if (qn && wr(w, ud, t->futicies[i].tids, qn * sizeof(uint32_t))) return -1;
	}
	if (wr(w, ud, "GuestThreadSet", 14)) return -1;
	return 0;
}

int mb_threads_load(mb_threads *t, mb_context *c, mb_read_cb r, uintptr_t ud) {
	if (t->active_tid != 1) { fprintf(stderr, "miniBox: thread hijack on load\n"); return -1; }
	char magic[14];
	if (rd(r, ud, magic, 14) || memcmp(magic, "GuestThreadSet", 14) != 0) return -1;
	if (rd(r, ud, &t->next_tid, 4) || rd(r, ud, &t->active_tid, 4)) return -1;
	uint32_t nt;
	if (rd(r, ud, &nt, 4)) return -1;
	t->nthreads = 0;
	for (uint32_t i = 0; i < nt; i++) { gthread g; if (rd(r, ud, &g, sizeof(gthread))) return -1; insert_thread(t, g); }
	for (size_t i = 0; i < t->nfut; i++) free(t->futicies[i].tids);
	t->nfut = 0;
	uint32_t nf;
	if (rd(r, ud, &nf, 4)) return -1;
	for (uint32_t i = 0; i < nf; i++) {
		uintptr_t addr; uint32_t qn;
		if (rd(r, ud, &addr, sizeof(uintptr_t)) || rd(r, ud, &qn, 4)) return -1;
		futex_queue *q = get_or_make_queue(t, addr);
		for (uint32_t j = 0; j < qn; j++) { uint32_t tid; if (rd(r, ud, &tid, 4)) return -1; queue_push(q, tid, 0); /* savestate predates deadlines: infinite (re-parked timed on retry) */ }
	}
	if (rd(r, ud, magic, 14) || memcmp(magic, "GuestThreadSet", 14) != 0) return -1;
	gthread *main = find_thread(t, 1);
	c->thread_area = main->thread_area; c->guest_rsp = main->rsp;
	return 0;
}
