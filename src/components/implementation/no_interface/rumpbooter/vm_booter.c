#include "vk_types.h"
#include "micro_booter.h"
#include "vk_api.h"
#include "cos2rk_types.h"
#include "rumpcalls.h"
#include <rk_inv_api.h>
#include <cos_kernel_api.h>
#include <cos_defkernel_api.h>
#include <cringbuf.h>
#include <sl.h>
#include <sl_thd.h>

extern void rump_booter_init(void *);
extern void timersub_init(void *);
extern void dlapp_init(void *);
extern int udpserv_main(void);
static struct cringbuf vmringbuf;
struct cringbuf *vmrb = NULL;

struct cos_defcompinfo *currdci = NULL;
struct cos_compinfo *currci = NULL;

vaddr_t shdmem_addr = 0;

/*
 * the capability for the thread switched to upon termination.
 * FIXME: not exit thread for now
 */
thdcap_t      termthd = BOOT_CAPTBL_SELF_INITTHD_BASE;
unsigned long tls_test[TEST_NTHDS];

#include <llprint.h>

/* For Div-by-zero test */
int num = 1, den = 0;

/* virtual machine id */
int vmid = 0;
int rumpns_vmid;

extern int cos_shmem_test(void);

cycles_t
hpet_first_period(void)
{
	int ret;
	static cycles_t start_period = 0;

	assert(currci);

	if (!start_period) {
		while ((ret = cos_introspect64(currci, BOOT_CAPTBL_SELF_INITHW_BASE, HW_GET_FIRST_HPET, &start_period)) == -EAGAIN) ;
		if (ret) assert(0);
	}

	return start_period;
}

void
vm_init(void *unused)
{
	int         rcvd, blocked;
	cycles_t    cycles;
	thdid_t     tid;
	tcap_time_t timeout = 0, thd_timeout;
	capid_t     cap_frontier = APP_CAPTBL_FREE;
	void (*init_fn)(void *) = NULL;

	currdci = cos_defcompinfo_curr_get();
	currci  = cos_compinfo_get(currdci);

	vmid = vk_vm_id();
	rumpns_vmid = vmid;
	cos_spdid_set(vmid);

	if (vmid == RUMP_SUB || vmid == DL_APP) {
		cringbuf_init(&vmringbuf, (void *)APP_SUB_SHM_BASE, APP_SUB_SHM_SZ);
		vmrb = &vmringbuf;
	} else {
		*((u32_t *)APP_SUB_SHM_BASE) = 0;
	}

	switch(vmid) {
	case RUMP_SUB: {
		init_fn = rump_booter_init;
		cap_frontier = RK_CAPTBL_FREE;
		break;
	}
	case TIMER_SUB: {
		init_fn = timersub_init;
		cap_frontier = TM_CAPTBL_FREE;
		break;
	}
	case DL_APP: {
		init_fn = dlapp_init;
		break;
	}
	case UDP_APP: {
		int shdmem_id = -1, rk_shdmem_id = -1;

		printc("Udp Application, allocating shared memory for RK\n");
		shdmem_id = shmem_allocate_invoke();
		assert(shdmem_id > -1);
		shdmem_addr = shmem_get_vaddr_invoke(shdmem_id);
		assert(shdmem_addr > 0);

		/* Sinv down to RK to map in shared mem page */
		rk_shdmem_id = rk_map_shdmem(shdmem_id, vmid);
		assert(rk_shdmem_id > -1);

		break;
	}
	default: assert(0);
	}

	cos_meminfo_init(&currci->mi, BOOT_MEM_KM_BASE, VM_UNTYPED_SIZE(vmid),
			BOOT_CAPTBL_SELF_UNTYPED_PT);

	cos_defcompinfo_init_ext(BOOT_CAPTBL_SELF_INITTCAP_BASE, BOOT_CAPTBL_SELF_INITTHD_BASE,
				 BOOT_CAPTBL_SELF_INITRCV_BASE, BOOT_CAPTBL_SELF_PT,
				 BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP, (vaddr_t)cos_get_heap_ptr(),
				 cap_frontier);

	PRINTC("RUNNING %s %d\n", vmid < APP_START_ID ? "VM" : "APP", vmid);
	if (init_fn == NULL) {
		PRINTC("Yes, UDP SERVER!\n");
		udpserv_main();
	} else {
		init_fn(NULL);
	}

	PRINTC("ERROR!!!");

	while (1) ;
}

