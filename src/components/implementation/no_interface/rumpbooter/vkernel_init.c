#include <cos_component.h>
#include <cobj_format.h>
#include <cos_kernel_api.h>
#include <vkern_api.h>
#include "cos_sync.h"
#include "vk_api.h"
#include "spin.h"

#define PRINT_FN prints
#define debug_print(str) (PRINT_FN(str __FILE__ ":" STR(__LINE__) ".\n"))
#define BUG() do { debug_print("BUG @ "); *((int *)0) = 0; } while (0);

thdcap_t vk_termthd; /* switch to this to shutdown */
extern void vm_init(void *);
extern vaddr_t cos_upcall_entry;
struct cos_compinfo vkern_info;
struct cos_compinfo vkern_shminfo;
unsigned int ready_vms = COS_VIRT_MACH_COUNT;

unsigned int cycs_per_usec = 1;
unsigned int cycs_per_msec = 1;

/*worker thds for dlvm*/
//extern void dl_work_one(void *);

/*
 * Init caps for each VM
 */
cycles_t total_credits;
tcap_t vminittcap[COS_VIRT_MACH_COUNT];
int vm_cr_reset[COS_VIRT_MACH_COUNT];
int vm_bootup[COS_VIRT_MACH_COUNT];
thdcap_t vm_main_thd[COS_VIRT_MACH_COUNT];
thdcap_t vm_exit_thd[COS_VIRT_MACH_COUNT];
thdid_t vm_main_thdid[COS_VIRT_MACH_COUNT];
arcvcap_t vminitrcv[COS_VIRT_MACH_COUNT];
asndcap_t vksndvm[COS_VIRT_MACH_COUNT];
tcap_res_t vmcredits[COS_VIRT_MACH_COUNT];
tcap_prio_t vmprio[COS_VIRT_MACH_COUNT];
int vmstatus[COS_VIRT_MACH_COUNT];
/* only two indices cause it should never have DL_VM in it */
int runqueue[COS_VIRT_MACH_COUNT-1];

/*
 * I/O transfer caps from VM0 <=> VMx
 */
thdcap_t vm0_io_thd[COS_VIRT_MACH_COUNT-1];
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
tcap_t vm0_io_tcap[COS_VIRT_MACH_COUNT-1];
#endif
arcvcap_t vm0_io_rcv[COS_VIRT_MACH_COUNT-1];
asndcap_t vm0_io_asnd[COS_VIRT_MACH_COUNT-1];
/*
 * I/O transfer caps from VMx <=> VM0
 */
thdcap_t vms_io_thd[COS_VIRT_MACH_COUNT-1];
arcvcap_t vms_io_rcv[COS_VIRT_MACH_COUNT-1];
asndcap_t vms_io_asnd[COS_VIRT_MACH_COUNT-1];

thdcap_t sched_thd;
tcap_t sched_tcap;
arcvcap_t sched_rcv;

asndcap_t chtoshsnd;
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
thdcap_t dom0_sla_thd;
tcap_t dom0_sla_tcap;
arcvcap_t dom0_sla_rcv;
asndcap_t dom0_sla_snd, vktoslasnd;
#endif

void
vk_term_fn(void *d)
{
	BUG();
}

#if defined(__INTELLIGENT_TCAPS__)
void
vk_time_fn(void *d) 
{
	while (1) {
		int pending = cos_rcv(vk_time_rcv[(int)d]);
		printc("vkernel: rcv'd from vm %d\n", (int)d);
	}
}

void
vm_time_fn(void *d)
{
	while (1) {
		int pending = cos_rcv(VM_CAPTBL_SELF_TIMERCV_BASE);
		printc("%d: rcv'd from vkernel\n", (int)d);
	}
}
#endif

extern int vmid;

void
vm0_io_fn(void *d) 
{
	int line;
	unsigned int irqline;
	arcvcap_t rcvcap;
	switch((int)d) {
		case DL_VM:
			line = 0;
			break;
		case 1:
			line = 13;
			irqline = IRQ_VM1;
			break;
		/*case 2:
			line = 15;
			irqline = IRQ_VM2;
			break;*/
		default: assert(0);
	}

	rcvcap = VM0_CAPTBL_SELF_IORCV_SET_BASE + (((int)d - 1) * CAP64B_IDSZ);
	while (1) {
		int pending = cos_rcv(rcvcap);
	//	tcap_res_t budget = (tcap_res_t)cos_introspect(&vkern_info, vm0_io_tcap[DL_VM-1], TCAP_GET_BUDGET);
	//       if(budget < 60000) printc("budget: %lu\n", budget);
	//	printc("line %d\n", (int)line);
		if (line == 0) {
			//	cos_switch(BOOT_CAPTBL_SELF_INITTHD_BASE, BOOT_CAPTBL_SELF_INITTCAP_BASE, DLVM_PRIO, 0, 0, cos_sched_sync());
			continue;
		}
		intr_start(irqline);
		bmk_isr(line);
		//cos_vio_tcap_set((int)d);
		intr_end();
	}
}

void
vmx_io_fn(void *d)
{
	assert((int)d != DL_VM);
	while (1) {
		int pending = cos_rcv(VM_CAPTBL_SELF_IORCV_BASE);
		//continue;
		intr_start(IRQ_DOM0_VM);
		bmk_isr(12);
		intr_end();
	}
}

void
setup_credits(void)
{
	int i;
	
	total_credits = 0;

	for (i = 0 ; i < COS_VIRT_MACH_COUNT ; i ++) {
		if (vmstatus[i] != VM_EXITED) {
			switch (i) {
				case 0:
					vmcredits[i] = (DOM0_CREDITS * VM_TIMESLICE * cycs_per_usec);
					total_credits += (DOM0_CREDITS * VM_TIMESLICE * cycs_per_usec);
					break;
				case 1:
					#ifdef GRAPHTP
						vmcredits[i] = TCAP_RES_INF;
					#else
						vmcredits[i] = (VM1_CREDITS * VM_TIMESLICE * cycs_per_usec);
					#endif
					
					total_credits += (VM1_CREDITS * VM_TIMESLICE * cycs_per_usec);
					break;
				case 2:
					vmcredits[i] = (VM2_CREDITS * VM_TIMESLICE * cycs_per_usec);
					//vmcredits[i] = (VM2_CREDITS * VM_TIMESLICE * cycs_per_usec);
					total_credits += (VM2_CREDITS * VM_TIMESLICE * cycs_per_usec);
					break;
				default: assert(0);
					vmcredits[i] = VM_TIMESLICE;
					break;
			}
		} 
	}
#ifdef GRAPHTP
	total_credits=10*cycs_per_msec;
#endif
}

void
reset_credits(void)
{
	struct vm_node *vm;
	int i;

	for (i = 0 ; i < COS_VIRT_MACH_COUNT ; i ++) {
		if (vmstatus[i] != VM_BLOCKED)
			vmstatus[i] = VM_RUNNING;
		vm_cr_reset[i] = 1;
	}
}

void
fillup_budgets(void)
{
//	vm_cr_reset[0] = 1;
//	vm_cr_reset[1] = 1;
//	vm_cr_reset[2] = 1;
	int i;
	vmprio[0] = DOM0_PRIO;
	vmprio[1] = NWVM_PRIO;
	vmprio[2] = DLVM_PRIO;
	
	for (i = 0 ; i < COS_VIRT_MACH_COUNT; i++) {
		vm_cr_reset[0] = 1;

		/* Never insert DL_VM in the runqueue */
		//if (vmprio[i] == PRIO_HIGH)     runqueue[0] = i;
		if (vmprio[i] == PRIO_MID)      runqueue[0] = i;
		else if (vmprio[i] == PRIO_LOW) runqueue[1] = i;
	}
}

/*
 * Avoiding credit based budgets at bootup.
 * Can be disabled by just setting this value to 0.
 *
 * A Vm1 with x budget > y of another vm2 slows down
 * bootup sequence because vm1 continuously blocks once
 * it's done with it's bootup..
 * Can see that visually.. This is just to fix that.
 * Credits at bootup shouldn't matter..!!
 */
int
bootup_sched_fn(int index)
{
	int i;

	assert(index != DL_VM);
	for (i = 0 ; i < BOOTUP_ITERS ; i ++) {
		tcap_res_t budget = (tcap_res_t)cos_introspect(&vkern_info, vminittcap[index], TCAP_GET_BUDGET);
		tcap_res_t timeslice = 5 * VM_TIMESLICE * cycs_per_usec;

		if (budget >= timeslice) {
			if (cos_asnd(vksndvm[index], 1)) assert(0);
		} else {
			if (cos_tcap_delegate(vksndvm[index], sched_tcap, timeslice - budget, vmprio[index], TCAP_DELEG_YIELD)) assert(0);
		}
	}
	printc("VM%d Bootup complete\n", index);

	return 1;
}

int
boot_dlvm_sched_fn(int index)
{
	tcap_res_t timeslice = 5 * VM_TIMESLICE * cycs_per_usec;
	tcap_res_t budget = (tcap_res_t)cos_introspect(&vkern_info, vminittcap[index], TCAP_GET_BUDGET);

	assert(index == DL_VM);

	if (budget > 0) return 1;
	printc("DLVM BOOT TRANSFER\n");
	if ((cos_tcap_transfer(vminitrcv[index], sched_tcap, timeslice-budget, vmprio[index]))) {
		printc("\tTcap transfer failed \n");
		assert(0);
	}
	/* ideal case */
	cos_switch(vm_main_thd[index], vminittcap[index], vmprio[index], 0, 0, 0);
	printc("VM%d Bootup complete\n", index);
	vmstatus[index] = VM_BLOCKED; //not run after first time in vkernel..

	return 1;
}

uint64_t t_vm_cycs  = 0;
uint64_t t_dom_cycs = 0;

static void
chk_replenish_budgets(void)
{
	int i;
	static cycles_t last_replenishment = 0;
	cycles_t now;

	rdtscll(now);

	//printc("Now %llu last: %llu ttl: %llu diff: %llu \n", now, last_replenishment, total_credits, now-last_replenishment);
	if (last_replenishment == 0 || (now - last_replenishment) >= total_credits) {

		rdtscll(last_replenishment);
		for (i = 0 ; i < COS_VIRT_MACH_COUNT ; i++) {
			tcap_res_t budget = 0, transfer_budget = 0;

			budget = (tcap_res_t)cos_introspect(&vkern_info, vminittcap[i], TCAP_GET_BUDGET);
			transfer_budget = vmcredits[i] - budget;

			if (TCAP_RES_IS_INF(budget) || budget > vmcredits[i]) continue;

			if ((cos_tcap_transfer(vminitrcv[i], sched_tcap, transfer_budget, vmprio[i]))) assert(0);
		}
	}
}

static void
wakeup_vms(unsigned x)
{
	static cycles_t last_wakeup = 0;
	cycles_t wakeupslice = VM_MS_TIMESLICE * x * cycs_per_msec;
	cycles_t now;
	
	rdtscll(now);
	if (last_wakeup == 0 || now - last_wakeup > wakeupslice) {
		int i;

		last_wakeup = now;
		/* Skips DLVM (should never be in VL runqueue) */
		for (i = 0 ; i < COS_VIRT_MACH_COUNT; i ++) {
			if (i == DL_VM || vmstatus[i] == VM_EXITED) continue;

			vmstatus[i] = VM_RUNNING;
		}
	}
}


#undef JUST_RR

static int
sched_vm(void)
{
#ifdef JUST_RR
	static int sched_index = DL_VM;
	int index = sched_index;

	sched_index ++;
	sched_index %= COS_VIRT_MACH_COUNT;

	return index;
#else
	int i;

	for (i = 0 ; i < COS_VIRT_MACH_COUNT-1 ; i++) {
		if (vmstatus[runqueue[i]] == VM_RUNNING) return runqueue[i];
	}

	return -1;
#endif
}

void
bootup_hack(void)
{
	bootup_sched_fn(1);
	bootup_sched_fn(0);
	boot_dlvm_sched_fn(2);

	printc("BOOTUP DONE\n");
}

#define YIELD_CYCS 10000
void
sched_fn(void *x)
{
	thdid_t tid;
	int blocked;
	cycles_t cycles;
	int index, j;
	tcap_res_t cycs;
	cycles_t total_cycles = 0;
	int no_vms = 0;
	int done_printing = 0;
	cycles_t cpu_usage[COS_VIRT_MACH_COUNT];
	cycles_t cpu_cycs[COS_VIRT_MACH_COUNT];
	unsigned int usage[COS_VIRT_MACH_COUNT];
	unsigned int count[COS_VIRT_MACH_COUNT];
	cycles_t cycs_per_sec                   = cycs_per_usec * 1000 * 1000;
	cycles_t total_cycs                     = 0;
	unsigned int counter                    = 0;
	cycles_t start                          = 0;
	cycles_t end                            = 0;
	
	memset(cpu_usage, 0, sizeof(cpu_usage));
	memset(cpu_cycs, 0, sizeof(cpu_cycs));
	(void)cycs;

	printc("Scheduling VMs(Rumpkernel contexts)....\n");
	memset(vm_bootup, 0, sizeof(vm_bootup));


	bootup_hack();	
	chk_replenish_budgets();
//	while(1);
	while (ready_vms) {
		//struct vm_node *x, *y;
		//unsigned int count_over = 0;
		//int ret;
		tcap_res_t budget = 0;
		//int send          = 1;
		int pending;
		tcap_res_t sched_budget = 0, transfer_budget = 0;
		tcap_res_t min_budget = VM_MIN_TIMESLICE * cycs_per_usec;
		int index = 0;
		
		wakeup_vms(1);
		do {
			int i;
			int rcvd = 0;

			pending = cos_sched_rcv_all(sched_rcv, &rcvd, &tid, &blocked, &cycles);
			if (!tid) continue;
			
			if (tid == vm_main_thdid[DL_VM]) continue; //don't worry if DL_VM is blocked on unblocked..
			for (i = 0; i < COS_VIRT_MACH_COUNT ; i++) {
				if (tid == vm_main_thdid[i]) {
					vmstatus[i] = blocked;
					break;
				}
			}
		} while (pending);
		
		
		chk_replenish_budgets();
		
		index = sched_vm();
#ifdef JUST_RR
		if (index == DL_VM || vmstatus[index] != VM_RUNNING) continue;
#else
		if (index < 0) continue;
		assert(index != DL_VM);
		assert(vmstatus[index] == VM_RUNNING);
#endif

		if (cos_asnd(vksndvm[index], 1)) assert(0);
	}
}

//#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
//cycles_t dom0_sla_act_cyc = 0;
//
//thdcap_t dummy_thd = 0;
//arcvcap_t dummy_rcv = 0;
//void
//dummy_fn(void *x)
//{ while (1) cos_rcv(dummy_rcv); } 
//
//void
//dom0_sla_fn(void *x)
//{
//	//static cycles_t prev = 0;
//	int vmid = 0;
//	struct cos_compinfo btr_info;
//
//	cos_meminfo_init(&btr_info.mi, BOOT_MEM_KM_BASE, COS_VIRT_MACH_MEM_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
//
//	cos_compinfo_init(&btr_info, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP,
//			  (vaddr_t)cos_get_heap_ptr(), VM0_CAPTBL_FREE, &btr_info);
//
//	while (1) {
//		tcap_res_t sla_budget;
//		thdid_t tid;
//		int blocked;
//		cycles_t cycles;//, now = 0, act = 0;
//		asndcap_t vm_snd;
//		//int i = 0;
//		
//		//cos_sched_rcv(VM0_CAPTBL_SELF_SLARCV_BASE, &tid, &blocked, &cycles);
//		sla_budget = (tcap_res_t)cos_introspect(&btr_info, VM0_CAPTBL_SELF_SLATCAP_BASE, TCAP_GET_BUDGET);
//		//rdtscll(now);
//		//act = (now - prev);
//		//prev = now;
//		
//		rdtscll(dom0_sla_act_cyc);
//		//printc("DOM0 SLA Activated: %llu : %lu : %llu\n", act, sla_budget, dom0_sla_act_cyc);
//
//		//if(sla_budget) if (cos_tcap_delegate(VM0_CAPTBL_SELF_SLASND_BASE, VM0_CAPTBL_SELF_SLATCAP_BASE, 0, vmprio[0], 0)) assert(0);
//		if(sla_budget) if (cos_tcap_transfer(BOOT_CAPTBL_SELF_INITRCV_BASE, VM0_CAPTBL_SELF_SLATCAP_BASE, 0, vmprio[0])) assert(0);
//	}
//}
//
//void
//chronos_fn(void *x)
//{
//	//static cycles_t prev = 0;
//
//	while (1) {
//		tcap_res_t total_budget = TCAP_RES_INF;
//		tcap_res_t sla_slice = VM_TIMESLICE * cycs_per_usec;
//		thdid_t tid;
//		int blocked;
//		cycles_t cycles;//, now = 0, act = 0;
//		struct vm_node *x, *y;
//
//		//cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_BASE, &tid, &blocked, &cycles);
//		//tcap_res_t sched_budget = (tcap_res_t)cos_introspect(&vkern_info, sched_tcap, TCAP_GET_BUDGET);
//		//tcap_res_t sla_budget = (tcap_res_t)cos_introspect(&vkern_info, dom0_sla_tcap, TCAP_GET_BUDGET);
//		//rdtscll(now);
//		//act = (now - prev);
//		//prev = now;
//		
///*		y = vm_next(&vms_runqueue);
//		assert(y != NULL);
//
//		x = y;
//		do {
//			int index = x->id;
//
//			total_budget += vmcredits[index];
//			x = vm_next(&vms_runqueue);
//
//		} while (x != y);
//		vm_prev(&vms_runqueue);
//*/
//		//total_budget *= SCHED_QUANTUM;
//		//printc("Chronos activated :%llu: %lu:%lu-%lu:%lu-%d\n", act, sched_budget, total_budget, sla_budget, sla_slice, SCHED_QUANTUM);
//
//		//if (cos_tcap_delegate(vktoslasnd, BOOT_CAPTBL_SELF_INITTCAP_BASE, sla_slice, PRIO_LOW, TCAP_DELEG_YIELD)) assert(0);
//		if (cos_tcap_transfer(dom0_sla_rcv, BOOT_CAPTBL_SELF_INITTCAP_BASE, sla_slice, PRIO_LOW)) assert(0);
//		//printc("%s:%d\n", __func__, __LINE__);
//		/* additional cycles so vk doesn't allocate less budget to one of the vms.. */
//		if (cos_tcap_delegate(chtoshsnd, BOOT_CAPTBL_SELF_INITTCAP_BASE, total_budget, PRIO_LOW, TCAP_DELEG_YIELD)) assert(0);
//		//rdtscll(act);
//		//printc("%llu:%llu:%llu\n", now, act, act - now);
//
//	}	
//}
//#endif

/* switch to vkernl booter thd */
void
vm_exit(void *id) 
{
	if (ready_vms > 1) assert((int)id); /* DOM0 cannot exit while other VMs are still running.. */

	/* basically remove from READY list */
	ready_vms --;
	vmstatus[(int)id] = VM_EXITED;
	/* do you want to spend time in printing? timer interrupt can screw with you, be careful */
	printc("VM %d Exiting\n", (int)id);
	while(1){}
	//while (1) cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_BASE);
	while (1) cos_thd_switch(sched_thd);
}

void
vkold_shmem_alloc(struct cos_compinfo *vmci, unsigned int id, unsigned long shm_sz)
{
	unsigned long shm_ptr = BOOT_MEM_SHM_BASE;
	vaddr_t src_pg = (shm_sz * id) + shm_ptr, dst_pg, addr;

	assert(vmci);
	assert(shm_ptr == round_up_to_pgd_page(shm_ptr));

	for (addr = shm_ptr ; addr < (shm_ptr + shm_sz) ; addr += PAGE_SIZE, src_pg += PAGE_SIZE) {
		/* VM0: mapping in all available shared memory. */
		src_pg = (vaddr_t)cos_page_bump_alloc(&vkern_shminfo);
		assert(src_pg && src_pg == addr);

		dst_pg = cos_mem_alias(vmci, &vkern_shminfo, src_pg);
		assert(dst_pg && dst_pg == addr);
	}	

	return;
}

void
vkold_shmem_map(struct cos_compinfo *vmci, unsigned int id, unsigned long shm_sz)
{
	unsigned long shm_ptr = BOOT_MEM_SHM_BASE;
	vaddr_t src_pg = (shm_sz * (id-1)) + shm_ptr, dst_pg, addr;

	assert(vmci);
	assert(shm_ptr == round_up_to_pgd_page(shm_ptr));

	for (addr = shm_ptr ; addr < (shm_ptr + shm_sz) ; addr += PAGE_SIZE, src_pg += PAGE_SIZE) {
		/* VMx: mapping in only a section of shared-memory to share with VM0 */
		assert(src_pg);

		dst_pg = cos_mem_alias(vmci, &vkern_shminfo, src_pg);
		assert(dst_pg && dst_pg == addr);
	}	

	return;
}

void
cos_init(void)
{
	struct cos_compinfo vmbooter_info[COS_VIRT_MACH_COUNT];
	struct cos_compinfo vmbooter_shminfo[COS_VIRT_MACH_COUNT];
	assert(COS_VIRT_MACH_COUNT >= 2);
	int test = 0;
	printc("Hypervisor:vkernel START\n");

	int i = 0, id = 0, cycs;
	int page_range = 0;

	while (!(cycs = cos_hw_cycles_per_usec(BOOT_CAPTBL_SELF_INITHW_BASE))) ;
	printc("\t%d cycles per microsecond\n", cycs);
	cycs_per_usec = (unsigned int)cycs;

	while (!(cycs = cos_hw_cycles_per_msec(BOOT_CAPTBL_SELF_INITHW_BASE))) ;
	printc("\t%d cycles per microsecond\n", cycs);
	cycs_per_msec = (unsigned int)cycs;
	
	printc("cycles_per_msec: %lu, TIME QUANTUM: %lu, RES_INF: %lu\n", (unsigned long)cycs_per_msec, (unsigned long)(VM_TIMESLICE*cycs_per_msec), TCAP_RES_INF);
	printc("cycles_per_usec: %lu, TIME QUANTUM: %lu, RES_INF: %lu\n", (unsigned long)cycs_per_usec, (unsigned long)(VM_TIMESLICE*cycs_per_usec), TCAP_RES_INF);

	spin_calib();
	//test_spinlib();

	vm_list_init();
	setup_credits();
	fillup_budgets();

	printc("Hypervisor:vkernel initializing\n");
	cos_meminfo_init(&vkern_info.mi, BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	cos_compinfo_init(&vkern_info, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP,
			(vaddr_t)cos_get_heap_ptr(), BOOT_CAPTBL_FREE, &vkern_info);
	cos_compinfo_init(&vkern_shminfo, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP,
			(vaddr_t)BOOT_MEM_SHM_BASE, BOOT_CAPTBL_FREE, &vkern_info);


	vk_termthd = cos_thd_alloc(&vkern_info, vkern_info.comp_cap, vk_term_fn, NULL);
	assert(vk_termthd);

#if defined(__SIMPLE_DISTRIBUTED_TCAPS__)
	printc("DISTRIBUTED TCAPS INFRA\n");
#else
	assert(0);
#endif

	printc("Initializing Timer/Scheduler Thread\n");
#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
//	sched_tcap = cos_tcap_alloc(&vkern_info);
//	assert(sched_tcap);
//	sched_thd = cos_thd_alloc(&vkern_info, vkern_info.comp_cap, sched_fn, (void *)0);
//	assert(sched_thd);
//	sched_rcv = cos_arcv_alloc(&vkern_info, sched_thd, sched_tcap, vkern_info.comp_cap, BOOT_CAPTBL_SELF_INITRCV_BASE);
//	assert(sched_rcv);
	sched_tcap = BOOT_CAPTBL_SELF_INITTCAP_BASE;
	sched_thd = BOOT_CAPTBL_SELF_INITTHD_BASE;
	sched_rcv = BOOT_CAPTBL_SELF_INITRCV_BASE;
#elif defined(__SIMPLE_XEN_LIKE_TCAPS__)
	sched_tcap = BOOT_CAPTBL_SELF_INITTCAP_BASE;
	sched_thd = BOOT_CAPTBL_SELF_INITTHD_BASE;
	sched_rcv = BOOT_CAPTBL_SELF_INITRCV_BASE;
#endif

	chtoshsnd = cos_asnd_alloc(&vkern_info, sched_rcv, vkern_info.captbl_cap);
	assert(chtoshsnd);

	for (id = 0; id < COS_VIRT_MACH_COUNT; id ++) {
		printc("VM %d Initialization Start\n", id);
		captblcap_t vmct;
		pgtblcap_t vmpt, vmutpt;
		compcap_t vmcc;
		int ret;

		printc("\tForking VM\n");
		vm_exit_thd[id] = cos_thd_alloc(&vkern_info, vkern_info.comp_cap, vm_exit, (void *)id);
		assert(vm_exit_thd[id]);

		test = (thdid_t)cos_introspect(&vkern_info, vm_exit_thd[id], THD_GET_TID);

		vmct = cos_captbl_alloc(&vkern_info);
		assert(vmct);

		vmpt = cos_pgtbl_alloc(&vkern_info);
		assert(vmpt);

		vmutpt = cos_pgtbl_alloc(&vkern_info);
		assert(vmutpt);

		vmcc = cos_comp_alloc(&vkern_info, vmct, vmpt, (vaddr_t)&cos_upcall_entry);
		assert(vmcc);

		cos_meminfo_init(&vmbooter_info[id].mi, 
				BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, vmutpt);
		if (id == 0) { 
			cos_compinfo_init(&vmbooter_shminfo[id], vmpt, vmct, vmcc,
					(vaddr_t)BOOT_MEM_SHM_BASE, VM0_CAPTBL_FREE, &vkern_info);
			cos_compinfo_init(&vmbooter_info[id], vmpt, vmct, vmcc,
					(vaddr_t)BOOT_MEM_VM_BASE, VM0_CAPTBL_FREE, &vkern_info);
		} else {
			cos_compinfo_init(&vmbooter_shminfo[id], vmpt, vmct, vmcc,
					(vaddr_t)BOOT_MEM_SHM_BASE, VM_CAPTBL_FREE, &vkern_info);
			cos_compinfo_init(&vmbooter_info[id], vmpt, vmct, vmcc,
					(vaddr_t)BOOT_MEM_VM_BASE, VM_CAPTBL_FREE, &vkern_info);
		}

		vm_main_thd[id] = cos_thd_alloc(&vkern_info, vmbooter_info[id].comp_cap, vm_init, (void *)id);
		assert(vm_main_thd[id]);
		vm_main_thdid[id] = (thdid_t)cos_introspect(&vkern_info, vm_main_thd[id], THD_GET_TID);
		test = vm_main_thdid[id];
		if (test == 12) {
				printc("THIS IT!! 740: %d , %d\n", id, test);
		}

		printc("\tMain thread= cap:%x tid:%x\n", (unsigned int)vm_main_thd[id], vm_main_thdid[id]);
		cos_cap_cpy_at(&vmbooter_info[id], BOOT_CAPTBL_SELF_INITTHD_BASE, &vkern_info, vm_main_thd[id]);
		vmstatus[id] = VM_RUNNING;

		/*
		 * Set some fixed mem pool requirement. 64MB - for ex. 
		 * Allocate as many pte's 
		 * Map contiguous untyped memory for that size to those PTE's 
		 * Set cos_meminfo for vm accordingly.
		 * cos_untyped_alloc(ci, size)
		 */

		printc("\tCopying required capabilities\n");
		cos_cap_cpy_at(&vmbooter_info[id], BOOT_CAPTBL_SELF_CT, &vkern_info, vmct);
		cos_cap_cpy_at(&vmbooter_info[id], BOOT_CAPTBL_SELF_PT, &vkern_info, vmpt);
		cos_cap_cpy_at(&vmbooter_info[id], BOOT_CAPTBL_SELF_UNTYPED_PT, &vkern_info, vmutpt);
		cos_cap_cpy_at(&vmbooter_info[id], BOOT_CAPTBL_SELF_COMP, &vkern_info, vmcc);
		/* 
		 * TODO: We need seperate such capabilities for each VM. Can't use the BOOTER ones. 
		 */
		cos_cap_cpy_at(&vmbooter_info[id], BOOT_CAPTBL_SELF_INITHW_BASE, &vkern_info, BOOT_CAPTBL_SELF_INITHW_BASE); 
		cos_cap_cpy_at(&vmbooter_info[id], VM_CAPTBL_SELF_EXITTHD_BASE, &vkern_info, vm_exit_thd[id]); 

		printc("\tCreating other required initial capabilities\n");
		vminittcap[id] = cos_tcap_alloc(&vkern_info);
		assert(vminittcap[id]);

		vminitrcv[id] = cos_arcv_alloc(&vkern_info, vm_main_thd[id], vminittcap[id], vkern_info.comp_cap, sched_rcv);
		assert(vminitrcv[id]);

		cos_cap_cpy_at(&vmbooter_info[id], BOOT_CAPTBL_SELF_INITRCV_BASE, &vkern_info, vminitrcv[id]);
		cos_cap_cpy_at(&vmbooter_info[id], BOOT_CAPTBL_SELF_INITTCAP_BASE, &vkern_info, vminittcap[id]);

		/*
		 * Create send end-point to each VM's INITRCV end-point for scheduling.
		 */
		vksndvm[id] = cos_asnd_alloc(&vkern_info, vminitrcv[id], vkern_info.captbl_cap);
		assert(vksndvm[id]);
//#if defined(__INTELLIGENT_TCAPS__) || defined(__SIMPLE_DISTRIBUTED_TCAPS__)
//		if (id == 0) {
//			printc("\tCreating Time Management System Capabilities in DOM0\n");
//			dom0_sla_tcap = cos_tcap_alloc(&vkern_info);
//			assert(dom0_sla_tcap);
//			dom0_sla_thd = cos_thd_alloc(&vkern_info, vmbooter_info[id].comp_cap, dom0_sla_fn, (void *)id);
//			assert(dom0_sla_thd);
//			test = (thdid_t)cos_introspect(&vkern_info, dom0_sla_thd, THD_GET_TID);
//
//			dom0_sla_rcv = cos_arcv_alloc(&vkern_info, dom0_sla_thd, dom0_sla_tcap, vkern_info.comp_cap, vminitrcv[id]);
//			assert(dom0_sla_rcv);
//
//			if (cos_cap_cpy_at(&vmbooter_info[id], VM0_CAPTBL_SELF_SLATCAP_BASE, &vkern_info, dom0_sla_tcap)) assert(0);
//			if (cos_cap_cpy_at(&vmbooter_info[id], VM0_CAPTBL_SELF_SLATHD_BASE, &vkern_info, dom0_sla_thd)) assert(0);
//			if (cos_cap_cpy_at(&vmbooter_info[id], VM0_CAPTBL_SELF_SLARCV_BASE, &vkern_info, dom0_sla_rcv)) assert(0);
//
//			dom0_sla_snd = vksndvm[id];
//			vktoslasnd = cos_asnd_alloc(&vkern_info, dom0_sla_rcv, vkern_info.captbl_cap);
//			assert(vktoslasnd);
//			if (cos_cap_cpy_at(&vmbooter_info[id], VM0_CAPTBL_SELF_SLASND_BASE, &vkern_info, dom0_sla_snd)) assert(0);
//
//			dummy_thd = cos_thd_alloc(&vkern_info, vkern_info.comp_cap, dummy_fn, (void *)id);
//			assert(dummy_thd);
//			test = (thdid_t)cos_introspect(&vkern_info, dummy_thd, THD_GET_TID);
//			if (test == 8) {
//				printc("THIS IT!! 790");
//				assert(0);
//			}
//			dummy_rcv = cos_arcv_alloc(&vkern_info, dummy_thd, dom0_sla_tcap, vkern_info.comp_cap, dom0_sla_rcv);
//			assert(dummy_rcv);
//		}
//#endif

		cos_cap_cpy_at(&vmbooter_info[id], VM_CAPTBL_SELF_VKASND_BASE, &vkern_info, chtoshsnd);

		if (id > 0) {
			/* DOM0 to have capability to delegate time to VM */
			cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_INITASND_SET_BASE + (id-1)*CAP64B_IDSZ, &vkern_info, vksndvm[id]);
			printc("\tSetting up Cross VM (between vm0 and vm%d) communication channels\n", id);
			if (id == DL_VM) {
				/* The HPET tcap in dom0 (used for vmio and hpet irq */	
				vm0_io_thd[id-1] = cos_thd_alloc(&vkern_info, vmbooter_info[0].comp_cap, vm0_io_fn, (void *)id);
				assert(vm0_io_thd[id-1]);
				vm0_io_tcap[id-1] = cos_tcap_alloc(&vkern_info);
				assert(vm0_io_tcap[id-1]);
				vm0_io_rcv[id-1] = cos_arcv_alloc(&vkern_info, vm0_io_thd[id-1], vm0_io_tcap[id-1], vkern_info.comp_cap, vminitrcv[0]);
				assert(vm0_io_rcv[id-1]);
				/* Changing to init thd of dl_vm
				 * DOM0 -> DL_VM asnd
				 */
				vm0_io_asnd[id-1] = cos_asnd_alloc(&vkern_info, vminitrcv[id], vkern_info.captbl_cap);
				assert(vm0_io_asnd[id-1]);
				vms_io_asnd[id-1] = cos_asnd_alloc(&vkern_info, vm0_io_rcv[id-1], vkern_info.captbl_cap);
				assert(vms_io_asnd[id-1]);
				cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_IOTHD_SET_BASE + (id-1)*CAP16B_IDSZ, &vkern_info, vm0_io_thd[id-1]);
				cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_IOTCAP_SET_BASE + (id-1)*CAP16B_IDSZ, &vkern_info, vm0_io_tcap[id-1]);
				cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_IORCV_SET_BASE + (id-1)*CAP64B_IDSZ, &vkern_info, vm0_io_rcv[id-1]);
				cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_IOASND_SET_BASE + (id-1)*CAP64B_IDSZ, &vkern_info, vm0_io_asnd[id-1]);
				cos_cap_cpy_at(&vmbooter_info[id], VM_CAPTBL_SELF_IOASND_BASE, &vkern_info, vms_io_asnd[id-1]);
			} else {
				/* VM0 to VMid */
				vm0_io_thd[id-1] = cos_thd_alloc(&vkern_info, vmbooter_info[0].comp_cap, vm0_io_fn, (void *)id);
				assert(vm0_io_thd[id-1]);
				vms_io_thd[id-1] = cos_thd_alloc(&vkern_info, vmbooter_info[id].comp_cap, vmx_io_fn, (void *)id);
				assert(vms_io_thd[id-1]);

				vm0_io_tcap[id-1] = vminittcap[0];
				assert(vm0_io_tcap[id-1]);
				vm0_io_rcv[id-1] = cos_arcv_alloc(&vkern_info, vm0_io_thd[id-1], vm0_io_tcap[id-1], vkern_info.comp_cap, vminitrcv[0]);
				assert(vm0_io_rcv[id-1]);

				vms_io_rcv[id-1] = cos_arcv_alloc(&vkern_info, vms_io_thd[id-1], vminittcap[id], vkern_info.comp_cap, vminitrcv[id]);
				assert(vms_io_rcv[id-1]);
				cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_IOTCAP_SET_BASE + (id-1)*CAP16B_IDSZ, &vkern_info, vm0_io_tcap[id-1]);

				vm0_io_asnd[id-1] = cos_asnd_alloc(&vkern_info, vms_io_rcv[id-1], vkern_info.captbl_cap);
				assert(vm0_io_asnd[id-1]);

				vms_io_asnd[id-1] = cos_asnd_alloc(&vkern_info, vm0_io_rcv[id-1], vkern_info.captbl_cap);
				assert(vms_io_asnd[id-1]);

				cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_IOTHD_SET_BASE + (id-1)*CAP16B_IDSZ, &vkern_info, vm0_io_thd[id-1]);
				cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_IORCV_SET_BASE + (id-1)*CAP64B_IDSZ, &vkern_info, vm0_io_rcv[id-1]);
				cos_cap_cpy_at(&vmbooter_info[0], VM0_CAPTBL_SELF_IOASND_SET_BASE + (id-1)*CAP64B_IDSZ, &vkern_info, vm0_io_asnd[id-1]);

				printc("VM_CAPTBL_SELF_IOASND_BASE%d\n", VM_CAPTBL_SELF_IOASND_BASE);
				cos_cap_cpy_at(&vmbooter_info[id], VM_CAPTBL_SELF_IOTHD_BASE, &vkern_info, vms_io_thd[id-1]);
				cos_cap_cpy_at(&vmbooter_info[id], VM_CAPTBL_SELF_IORCV_BASE, &vkern_info, vms_io_rcv[id-1]);
				cos_cap_cpy_at(&vmbooter_info[id], VM_CAPTBL_SELF_IOASND_BASE, &vkern_info, vms_io_asnd[id-1]);
			}
		}

		/*
		 * Create a new memory hole
		 * Copy as much memory as vkernel has typed.. 
		 * Map untyped memory to vkernel
		 */
		page_range = ((int)cos_get_heap_ptr() - BOOT_MEM_VM_BASE);
		if (id != 0) {
			/* 
			 * Map the VM0's Virtual Memory only after I/O Comm Caps are allocated with all other VMs.
			 * Basically, when we're done with the last VM's INIT. (we could do it outside the Loop too.)
			 */
			if (id == COS_VIRT_MACH_COUNT - 1) {
				printc("\tMapping in Booter component Virtual memory to VM0, Range: %u\n", page_range);
				for (i = 0; i < page_range; i += PAGE_SIZE) {
					// allocate page
					vaddr_t spg = (vaddr_t) cos_page_bump_alloc(&vkern_info);
					// copy mem - can even do it after creating and copying all pages.
					memcpy((void *) spg, (void *) (BOOT_MEM_VM_BASE + i), PAGE_SIZE);
					// copy cap
					vaddr_t dpg = cos_mem_alias(&vmbooter_info[0], &vkern_info, spg);
				}

			}
			printc("\tMapping in Booter component Virtual memory to VM%d, Range: %u\n", id, page_range);
			for (i = 0; i < page_range; i += PAGE_SIZE) {
				// allocate page
				vaddr_t spg = (vaddr_t) cos_page_bump_alloc(&vkern_info);
				// copy mem - can even do it after creating and copying all pages.
				memcpy((void *) spg, (void *) (BOOT_MEM_VM_BASE + i), PAGE_SIZE);
				// copy cap
				vaddr_t dpg = cos_mem_alias(&vmbooter_info[id], &vkern_info, spg);
			}
		}

		if (id == 0) {

			printc("\tCreating shared memory region from %x size %x\n", BOOT_MEM_SHM_BASE, COS_SHM_ALL_SZ);
			
			vkold_shmem_alloc(&vmbooter_shminfo[id], id, COS_SHM_ALL_SZ + ((sizeof(struct cos_shm_rb *)*2)*(COS_VIRT_MACH_COUNT-1)) );
			for(i = 1; i < (COS_VIRT_MACH_COUNT); i++){
				printc("\tInitializing ringbufs for sending\n");
				struct cos_shm_rb *sm_rb = NULL;	
				vk_send_rb_create(sm_rb, i);
			}

			//allocating ring buffers for recving data
			for(i = 1; i < (COS_VIRT_MACH_COUNT); i++){
				printc("\tInitializing ringbufs for rcving\n");
				struct cos_shm_rb *sm_rb_r = NULL;	
				vk_recv_rb_create(sm_rb_r, i);
			}

		} else {
			printc("\tMapping shared memory region from %x size %x\n", BOOT_MEM_SHM_BASE, COS_SHM_VM_SZ);
			vkold_shmem_map(&vmbooter_shminfo[id], id, COS_SHM_VM_SZ);
		}

		printc("\tAllocating/Partitioning Untyped memory\n");
		cos_meminfo_alloc(&vmbooter_info[id], BOOT_MEM_KM_BASE, COS_VIRT_MACH_MEM_SZ);

		printc("VM %d Init DONE\n", id);
	}

	//printc("sm_rb addr: %x\n", vk_shmem_addr_recv(2));
	printc("------------------[ Hypervisor & VMs init complete ]------------------\n");

	/* should I switch to scheduler ?? */
//	cos_switch(sched_thd, BOOT_CAPTBL_SELF_INITTCAP_BASE, PRIO_LOW, TCAP_TIME_NIL, 0, cos_sched_sync());

//	chronos_fn(NULL);
	sched_fn(NULL);

	printc("Hypervisor:vkernel END\n");
	cos_thd_switch(vk_termthd);
	printc("DEAD END\n");

	return;
}
