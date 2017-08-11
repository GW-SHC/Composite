#include <stdio.h>
#include <string.h>
#include <cos_component.h>
#include <cos_alloc.h>
#include <cos_kernel_api.h>
#include <cos_types.h>

#include "rumpcalls.h"
#include "rump_cos_alloc.h"
#include "cos_sched.h"
//#include "cos_lock.h"
#include "vkern_api.h"
//#define FP_CHECK(void(*a)()) ( (a == null) ? printc("SCHED: ERROR, function pointer is null.>>>>>>>>>>>\n");: printc("nothing");)
#include "cos_sync.h"

extern struct cos_compinfo booter_info;
extern struct cos_rumpcalls crcalls;

/* Thread cap */
volatile thdcap_t cos_cur = BOOT_CAPTBL_SELF_INITTHD_BASE;
volatile unsigned int cos_cur_tcap = BOOT_CAPTBL_SELF_INITTCAP_BASE;

#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
tcap_prio_t rk_thd_prio = RK_THD_PRIO;
#elif defined(__SIMPLE_XEN_LIKE_TCAPS__)
tcap_prio_t rk_thd_prio = PRIO_UNDER;
#endif

/* Mapping the functions from rumpkernel to composite */
void
cos2rump_setup(void)
{
	rump_bmk_memsize_init();

	crcalls.rump_cpu_clock_now 		= cos_cpu_clock_now;
	crcalls.rump_vm_clock_now 		= cos_vm_clock_now;
	crcalls.rump_cos_print 	      		= cos_print;
	crcalls.rump_vsnprintf        		= vsnprintf;
	crcalls.rump_strcmp           		= strcmp;
	crcalls.rump_strncpy          		= strncpy;

	/* These should be removed, confirm that they are never used */
	crcalls.rump_memcalloc        		= cos_memcalloc;
	crcalls.rump_memalloc         		= cos_memalloc;


	crcalls.rump_cos_thdid        		= cos_thdid;
	crcalls.rump_memcpy           		= memcpy;
	crcalls.rump_memset			= cos_memset;
	crcalls.rump_cpu_sched_create 		= cos_cpu_sched_create;

	if(!crcalls.rump_cpu_sched_create) printc("SCHED: rump_cpu_sched_create is set to null");

	crcalls.rump_cpu_sched_switch_viathd    = cos_cpu_sched_switch;
	crcalls.rump_memfree			= cos_memfree;
	crcalls.rump_tls_init 			= cos_tls_init;
	crcalls.rump_va2pa			= cos_vatpa;
	crcalls.rump_pa2va			= cos_pa2va;
	crcalls.rump_resume                     = cos_resume;
	crcalls.rump_platform_exit		= cos_vm_exit;

	crcalls.rump_intr_enable		= intr_enable;
	crcalls.rump_intr_disable		= intr_disable;
	crcalls.rump_sched_yield		= cos_sched_yield;
	crcalls.rump_vm_yield			= cos_vm_yield;

	crcalls.rump_shmem_send			= cos_shmem_send;
	crcalls.rump_shmem_recv			= cos_shmem_recv;
	crcalls.rump_dequeue_size		= cos_dequeue_size;

	return;
}

int
cos_dequeue_size(unsigned int srcvm, unsigned int curvm)
{
	return vk_dequeue_size(srcvm, curvm);
}

/*rk shared mem functions*/
int
cos_shmem_send(void * buff, unsigned int size, unsigned int srcvm, unsigned int dstvm){

	asndcap_t sndcap;
	int ret;

	if(srcvm == 0) sndcap = VM0_CAPTBL_SELF_IOASND_SET_BASE + (dstvm - 1) * CAP64B_IDSZ;
	else sndcap = VM_CAPTBL_SELF_IOASND_BASE;

	//printc("%s = s:%d d:%d\n", __func__, srcvm, dstvm);
	cos_shm_write(buff, size, srcvm, dstvm);	

#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
	/* DOM0 just sends out the packets.. */
	if (!srcvm) {
		/* TODO: Before sending a event to the VM, first see if we can account for the time spent in i/o  processing */
		if(cos_asnd(sndcap, 0)) assert(0);

		/* deficit accounting.. for now: round robin between tcaps */
		cos_vio_tcap_update(dstvm);
	}
	/* VMs send out the packet and time to process the packet - All remaining budget in Tcap */
	else {
		tcap_res_t quantum = VM_TIMESLICE * cycs_per_usec;
		tcap_res_t min     = VIO_BUDGET_APPROX * cycs_per_usec;
		tcap_res_t budget = (tcap_res_t)cos_introspect(&booter_info, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_GET_BUDGET);
		tcap_res_t res;

		if (budget >= min) res = budget / 2; /* x cycles */ 
		else res = 0; /* 0 = 100% budget */

		if(cos_tcap_delegate(sndcap, BOOT_CAPTBL_SELF_INITTCAP_BASE, res, VIO_PRIO, 0)) assert(0);
	}
#elif defined(__SIMPLE_XEN_LIKE_TCAPS__)
	if(cos_asnd(sndcap, 0)) assert(0);
#endif
	return 1;
}

int
cos_shmem_recv(void * buff, unsigned int srcvm, unsigned int curvm){
	//printc("%s = s:%d d:%d\n", __func__, srcvm, curvm);
	return cos_shm_read(buff, srcvm, curvm);
}

/* send and recieve notifications */
void
rump2cos_rcv(void)
{
	printc("rump2cos_rcv");	
	return;
}

/* irq */
void
cos_irqthd_handler(void *line)
{
	int which = (int)line;
	arcvcap_t arcvcap = irq_arcvcap[which];
	
	while(1) {
		int pending = cos_rcv(arcvcap);

		intr_start(which);

		bmk_isr(which);

		intr_end();
	}
}

/* Memory */
extern unsigned long bmk_memsize;
void
rump_bmk_memsize_init(void)
{
	/* (1<<20) == 1 MG */
	bmk_memsize = COS_VIRT_MACH_MEM_SZ - ((1<<20)*2);
	printc("FIX ME: ");
	printc("bmk_memsize: %lu\n", bmk_memsize);
}

void
cos_memfree(void *cp)
{
	rump_cos_free(cp);
}

void *
cos_memcalloc(size_t n, size_t size)
{

	printc("cos_memcalloc was called\n");
	while(1);

	void *rv;
	size_t tot = n * size;

	if (size != 0 && tot / size != n)
		return NULL;

	rv = rump_cos_calloc(n, size);
	return rv;
}

void *
cos_memalloc(size_t nbytes, size_t align)
{
	printc("cos_memalloc was called\n");
	while(1);

	void *rv;

	rv = rump_cos_malloc(nbytes);

	return rv;
}

/*---- Scheduling ----*/
int boot_thd = BOOT_CAPTBL_SELF_INITTHD_BASE;

void
cos_tls_init(unsigned long tp, thdcap_t tc)
{
	cos_thd_mod(&booter_info, tc, (void *)tp);
}

void
cos_cpu_sched_create(struct bmk_thread *thread, struct bmk_tcb *tcb,
		void (*f)(void *), void *arg,
		void *stack_base, unsigned long stack_size)
{

	thdcap_t newthd_cap;
	int ret;

	newthd_cap = cos_thd_alloc(&booter_info, booter_info.comp_cap, f, arg);
	assert(newthd_cap);
	set_cos_thddata(thread, newthd_cap, cos_introspect(&booter_info, newthd_cap, 9));
}

extern int vmid;

static inline void
intr_switch(void)
{
	int ret, i = 32;

	if (!intrs) return;

	/* Man this is ugly...FIXME */
	for (; i > 0 ; i--) {
		int tmp = intrs;

		if ((tmp>>(i-1)) & 1) {
			do {
				ret = cos_switch(irq_thdcap[i], intr_eligible_tcap(i), irq_prio[i], TCAP_TIME_NIL, BOOT_CAPTBL_SELF_INITRCV_BASE, cos_sched_sync());
				assert (ret == 0 || ret == -EAGAIN);
			} while (ret == -EAGAIN);
		}
	}
}

#define CHECK_ITER 32
static inline void
check_vio_budgets(void)
{
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
	int i;
	static int iters;

	if (vmid) return;

	iters ++;
	if (iters != CHECK_ITER) return;
	iters = 0;

	for ( i = 1 ; i < COS_VIRT_MACH_COUNT ; i ++) {
		tcap_res_t budget;
		tcap_t tcp;
		asndcap_t snd;
		int j;
		tcap_res_t budg_max = VIO_BUDGET_MAX * cycs_per_usec;
		tcap_res_t budg_thr = VIO_BUDGET_THR * cycs_per_usec;
		tcp = vio_tcap[i - 1];

		if (i == CPU_BOUND_VM) continue;
		/*
		 * Deficit correction:
		 * 	Only deficit checks between vms.. 
		 * 	DOM0 deficit accounting - TODO
		 */
		for (j = i + 1 ; j < COS_VIRT_MACH_COUNT ; j ++) {
			unsigned int num = 0;
			int from, to;
			unsigned int fval, tval;

			from = i - 1;
			to = j - 1;
			assert (from < (COS_VIRT_MACH_COUNT - 1));
			assert (to < (COS_VIRT_MACH_COUNT - 1));
			fval = vio_deficit[from][to];
			tval = vio_deficit[to][from];

			num = (fval >= tval ? fval - tval : tval - fval);
			__sync_fetch_and_sub(&(vio_deficit[from][to]), fval);
			__sync_fetch_and_sub(&(vio_deficit[to][from]), tval);
			if (fval >= tval) {
				__sync_fetch_and_add(&(vio_deficit[from][to]), num);
			} else {
				__sync_fetch_and_add(&(vio_deficit[to][from]), num);
			}
		}

		/*
		 * I've more than required budget and I've enough to transfer,
		 * If I don't have this check, I might be trasferring in very small chunks.. 
		 */
		budget = (tcap_res_t)cos_introspect(&booter_info, tcp, TCAP_GET_BUDGET);
		if (budget >= budg_max && ((budget - budg_max) >= budg_thr)) {
			tcap_res_t bud = (budget - budg_max);			

			snd = VM0_CAPTBL_SELF_INITASND_SET_BASE + ((i - 1) * CAP64B_IDSZ);

			if (cos_tcap_delegate(snd, tcp, bud, PRIO_LOW, 0)) assert(0);
		} 
	}
#elif defined(__SIMPLE_XEN_LIKE_TCAPS__)
	return;
#endif
}

static void
cpu_bound_thd_fn(void *d)
{
	cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_BASE);

	while (1) {
		cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_BASE);
	}
}

static void
cpu_bound_test(void)
{
#if defined (__SIMPLE_DISTRIBUTED_TCAPS__)
#define BOUND_TEST_ITERS (1<<30)
	thdcap_t ts;
	int i = BOUND_TEST_ITERS;

	if (vmid != CPU_BOUND_VM) return;

	printc("I'm A CPU BOUND VM..Just SPINNING and printing \".\" every %d iters\n", BOUND_TEST_ITERS);
	while (1) {
		i = BOUND_TEST_ITERS;
		while (i > 0) i --;
		printc(".");
	}
	assert(0);
#endif
	return;
}

static void
print_cycles(void)
{
	static cycles_t total_cycles = 0;
	static cycles_t prev = 0, curr = 0;
	cycles_t cycs_per_sec = cycs_per_usec * 1000 * 1000 * 5;
	tcap_res_t isrbud, viobud, mainbud;

	rdtscll(curr);
	if (prev) total_cycles += (curr - prev);
	prev = curr;

	if (total_cycles >= cycs_per_sec) {
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
		if (vmid == IO_BOUND_VM) {
			mainbud = (tcap_res_t)cos_introspect(&booter_info, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_GET_BUDGET);
			
			printc("vm%d: %lu\n", vmid, mainbud);
		} else if (vmid == 0) {
			mainbud = (tcap_res_t)cos_introspect(&booter_info, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_GET_BUDGET);
			isrbud  = (tcap_res_t)cos_introspect(&booter_info, irq_tcap[HW_ISR_FIRST], TCAP_GET_BUDGET);
			viobud  = (tcap_res_t)cos_introspect(&booter_info, vio_tcap[IO_BOUND_VM - 1], TCAP_GET_BUDGET);
			printc("dom0: %lu\n", mainbud + isrbud + viobud);
			printc("dom0: %lu\n", mainbud);
		} else {
			assert(0);
		}
#elif defined(__SIMPLE_XEN_LIKE_TCAPS__)
		mainbud = (tcap_res_t)cos_introspect(&booter_info, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_GET_BUDGET);
		printc("vm%d: %lu\n", vmid, mainbud);
#endif

		total_cycles = 0;
	}
}

/* Called once from RK init thread. The one in while(1) */
void
cos_resume(void)
{
	/* this will not return if this vm is set to be CPU bound */
	cpu_bound_test();

	while(1) {
		int ret, first = 1;
		unsigned int rk_disabled;
		unsigned int intr_disabled;

		do {
			unsigned int contending;
			cycles_t cycles;
			int pending, blocked, irq_line;
			thdid_t tid;

			/*
			 * Handle all possible interrupts when
			 * interrupts are enabled or when
			 * a cos interrupt thread has disabled interrupts.
			 * Otherwise a rk thread disabled them and we need to
			 * switch back so it can enable interrupts
			 *
			 * Loop is neccessary incase we get preempted before a valid
			 * interrupt finishes execuing and we requrie that it finishes
			 * executing before returning to RK
		 	 */

			do {
				pending = cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_BASE, &tid, &blocked, &cycles);
				assert(pending <= 1);

				irq_line = intr_translate_thdid2irq(tid);
				intr_update(irq_line, blocked);

				if(first) {
					isr_get(cos_isr, &rk_disabled, &intr_disabled, &contending);
					if(rk_disabled && !intr_disabled) goto rk_resume;
					first = 0;
				}
			} while(pending);

			/*
			 * Done processing pending events
			 * Finish any remaining interrupts
			 */
			intr_switch();

		} while(intrs);

		assert(!intrs);

rk_resume:
		do {
			if(intr_disabled) break;
			cos_find_vio_tcap();
			/* TODO: decide which TCAP to use for rest of RK processing for I/O and do deficit accounting */
			ret = cos_switch(cos_cur, COS_CUR_TCAP, rk_thd_prio, TCAP_TIME_NIL, BOOT_CAPTBL_SELF_INITRCV_BASE, cos_sched_sync());
			assert(ret == 0 || ret == -EAGAIN);
		} while(ret == -EAGAIN);

		check_vio_budgets();
//		print_cycles();
	}
}

void
cos_cpu_sched_switch(struct bmk_thread *unsused, struct bmk_thread *next)
{
	sched_tok_t tok = cos_sched_sync();
	thdcap_t temp   = get_cos_thdcap(next);
	int ret;

	if(cos_isr) printc("%x\n", (unsigned int)cos_isr);
	assert(!cos_isr);
	cos_cur = temp;

	do {
		ret = cos_switch(cos_cur, COS_CUR_TCAP, rk_thd_prio, TCAP_TIME_NIL, BOOT_CAPTBL_SELF_INITRCV_BASE, tok);
		assert(ret == 0 || ret == -EAGAIN);
		if (ret == -EAGAIN) {
			/*
			 * I was preempted after getting the token and before updating cos_cur which just outdated my sched token
			 * So get a new token and try cos_switch again
			 * 
			 * And cos_cur == temp, can only happen if I've updated cos_cur and there were no other RK threads switched-to after that.
			 */
			if (cos_cur == temp) tok = cos_sched_sync();
			/*
			 * cos_cur is set to 'me' by some other RK thread because I was preempted after updating cos_cur
			 * ignore -EAGAIN in this scenario
			 */
			else break;
		}
	} while (ret == -EAGAIN);
}

/* --------- Timer ----------- */

/* Get the number of cycles in a single micro second. If we do nano second we lose the fraction */
//long long cycles_us = (long long)(CPU_GHZ * 1000);

/* Return monotonic time since RK per VM initiation in nanoseconds */
extern uint64_t t_vm_cycs;
extern uint64_t t_dom_cycs;
long long
cos_vm_clock_now(void)
{
	uint64_t tsc_now = 0;
	unsigned long long curtime = 0;
        
	assert(vmid <= 1);
	if (vmid == 0)      tsc_now = t_dom_cycs;
	else if (vmid == 1) tsc_now = t_vm_cycs;
	
	curtime = (long long)(tsc_now / cycs_per_usec); /* cycles to micro seconds */
        curtime = (long long)(curtime * 1000); /* micro to nano seconds */

	return curtime;
}

/* Return monotonic time since RK initiation in nanoseconds */
long long
cos_cpu_clock_now(void)
{
	uint64_t tsc_now = 0;
	unsigned long long curtime = 0;
        rdtscll(tsc_now);

       	/* We divide as we have cycles and cycles per micro second */
        curtime = (long long)(tsc_now / cycs_per_usec); /* cycles to micro seconds */
        curtime = (long long)(curtime * 1000); /* micro to nano seconds */
      

	return curtime;
}

void *
cos_vatpa(void * vaddr)
{ return cos_va2pa(&booter_info, vaddr); }

void *
cos_pa2va(void * pa, unsigned long len) 
{ return (void *)cos_hw_map(&booter_info, BOOT_CAPTBL_SELF_INITHW_BASE, (paddr_t)pa, (unsigned int)len); }

void
cos_vm_exit(void)
{ while (1) cos_thd_switch(VM_CAPTBL_SELF_EXITTHD_BASE); }

void
cos_sched_yield(void)
{ cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_BASE); }

void
cos_vm_yield(void)
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
{ if(cos_tcap_delegate(VM_CAPTBL_SELF_VKASND_BASE, BOOT_CAPTBL_SELF_INITTCAP_BASE, 0, PRIO_LOW, TCAP_DELEG_YIELD)) assert(0); }
#elif defined(__SIMPLE_XEN_LIKE_TCAPS__)
{ cos_asnd(VM_CAPTBL_SELF_VKASND_BASE, 1); }
#endif

void
cos_dom02io_transfer(unsigned int irqline, tcap_t tc, arcvcap_t rc, tcap_prio_t prio)
{
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
	tcap_res_t res = (VIO_BUDGET_APPROX * cycs_per_usec);
	tcap_res_t min_slice = (VM_MIN_TIMESLICE * cycs_per_usec);
	tcap_res_t initbudget = (tcap_res_t)cos_introspect(&booter_info, BOOT_CAPTBL_SELF_INITTCAP_BASE, TCAP_GET_BUDGET);	
	tcap_res_t irqbudget;
	int ret;

	assert (vmid == 0);

	if (irqline == IRQ_VM1 || irqline == IRQ_VM2) {
		if (initbudget >= res + min_slice) irqbudget = res;
		else                               irqbudget = 0;
	} else {
		if (initbudget >= res + min_slice) irqbudget = initbudget / 2;
		else                               irqbudget = 0;
	}
	if ((ret = cos_tcap_transfer(rc, BOOT_CAPTBL_SELF_INITTCAP_BASE, irqbudget, prio))) {
		printc("vio %d Tcap transfer failed %d\n", irqline, ret);
		assert(0);
	}

	switch(irqline) {
	case IRQ_VM1: dom0_vio_deficit[0] ++; break;
	case IRQ_VM2: dom0_vio_deficit[1] ++; break;

	default: break;
	}
#endif
}

void
cos_vio_tcap_set(unsigned int src)
{
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
	unsigned int use = (unsigned int) (cos_cur_tcap >> 16);
	unsigned int final, tmp;

	if (vmid) return;

	assert ((use < (COS_VIRT_MACH_COUNT-1)) && (src > 0 && src < COS_VIRT_MACH_COUNT));
	if (use != (src - 1)) {
		printc("%s:%d - use:%d src:%d\n", __func__, __LINE__, use, src - 1);
		/*
		 * if src is in deficit due to use.. then let use continue.
		 */
		if (vio_deficit[use][src - 1] < vio_deficit[src - 1][use]) return;

		use = src - 1;
		do {
			tmp = cos_cur_tcap;
			final = (use << 16) | ((vio_tcap[use] << 16) >> 16);

		} while (unlikely(!ps_cas((unsigned long *)&cos_cur_tcap, tmp, final)));
	}
#endif
}

void
cos_vio_tcap_update(unsigned int dst)
{
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
	unsigned int use = (unsigned int) (cos_cur_tcap >> 16);
	unsigned int final, tmp;
	static unsigned int counter = 0;

	if (vmid) return;

	assert ((use < (COS_VIRT_MACH_COUNT-1)) && (dst > 0 && dst < COS_VIRT_MACH_COUNT));
	if (use != (dst - 1)) {
		printc("%s:%d - use:%d dst:%d\n", __func__, __LINE__, use, dst - 1);
		__sync_fetch_and_add(&(vio_deficit[use][dst-1]), 1);

		if (vio_deficit[use][dst - 1] < vio_deficit[dst - 1][use]) return;

		use ++;
		use %= (COS_VIRT_MACH_COUNT - 1);
		do {
			tmp = cos_cur_tcap;
			final = (use << 16) | ((vio_tcap[use] << 16) >> 16);

		} while (unlikely(!ps_cas((unsigned long *)&cos_cur_tcap, tmp, final)));
	}
#endif
}

tcap_t
cos_find_vio_tcap(void)
{
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
	tcap_res_t irqbudget, initbudget;
	int i = 0, ret;
	unsigned int tmp, final;
	unsigned int use, using;
	tcap_t tcuse;

	if (vmid) return irq_tcap[IRQ_DOM0_VM];

	use = using = cos_cur_tcap >> 16;
	tcuse = (cos_cur_tcap << 16) >> 16;
	assert (use < (COS_VIRT_MACH_COUNT-1));

	irqbudget = (tcap_res_t)cos_introspect(&booter_info, vio_tcap[use], TCAP_GET_BUDGET);
/*	while (!irqbudget) {
		use ++;
		use %= (COS_VIRT_MACH_COUNT - 1);
		irqbudget = (tcap_res_t)cos_introspect(&booter_info, vio_tcap[use], TCAP_GET_BUDGET);
		if (i == (COS_VIRT_MACH_COUNT - 1)) break;
		i ++;
	}
*/
	
	if (!irqbudget) { // && (i == (COS_VIRT_MACH_COUNT - 1))) {
		cos_dom02io_transfer(use == 0 ? IRQ_VM1 : IRQ_VM2, vio_tcap[use], vio_rcv[use], vio_prio[use]); 
	}

	if (using != use || tcuse != vio_tcap[use]) {
		printc("%s:%d - use:%d using:%d\n", __func__, __LINE__, use, using);
		do {
			tmp = cos_cur_tcap;
			final = (use << 16) | ((vio_tcap[use] << 16) >> 16);

		} while (unlikely(!ps_cas((unsigned long *)&cos_cur_tcap, tmp, final)));
	}

	return COS_CUR_TCAP;
#elif defined(__SIMPLE_XEN_LIKE_TCAPS__)
	return BOOT_CAPTBL_SELF_INITTCAP_BASE;
#endif
}
