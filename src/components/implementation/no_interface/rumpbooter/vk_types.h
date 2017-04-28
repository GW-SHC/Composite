#ifndef VK_TYPES_H
#define VK_TYPES_H

#include <cos_types.h>

#define COS_VIRT_MACH_COUNT 3
#define COS_VIRT_MACH_MEM_SZ (1<<27) //128MB
#define COS_SHM_VM_SZ (1<<20) //2MB
#define COS_SHM_ALL_SZ (((COS_VIRT_MACH_COUNT - 1) > 0 ? (COS_VIRT_MACH_COUNT - 1) : 1) * COS_SHM_VM_SZ) //shared regions with VM 0

#define DL_VM 2

#define VM_MS_TIMESLICE 1*1000//*cycs_per_usec = 1ms
#define VM_TIMESLICE 1*1000//*cycs_per_usec = 1ms
#define VM_MIN_TIMESLICE (10) //1us
#define SCHED_MIN_TIMESLICE (10)
#define SCHED_QUANTUM (VM_TIMESLICE * 100)

#define VK_CYCS_DIFF_THRESH (1<<8)

#undef PRINT_CPU_USAGE 
#define MIN_CYCS (1<<12)

#define BOOTUP_ITERS 1000 

#define __SIMPLE_DISTRIBUTED_TCAPS__

#define HW_ISR_LINES 32
#define HW_ISR_FIRST 0

capid_t irq_thdcap[HW_ISR_LINES]; 
thdid_t irq_thdid[HW_ISR_LINES];
tcap_t irq_tcap[HW_ISR_LINES]; 
capid_t irq_arcvcap[HW_ISR_LINES];
tcap_prio_t irq_prio[HW_ISR_LINES];

enum vm_prio {
	PRIO_HIGH  = TCAP_PRIO_MAX,
	PRIO_LOW   = TCAP_PRIO_MAX+2,
	PRIO_MID   = TCAP_PRIO_MAX+1,
};

#define HPET_PERIOD_MS 10
#define HPET_PERIOD_US (HPET_PERIOD_MS*1000)

#define DLVM_PRIO PRIO_HIGH
#define NWVM_PRIO PRIO_LOW
#define DOM0_PRIO PRIO_MID

#define RIO_PRIO    PRIO_HIGH /* REAL I/O Priority */
#define VIO_PRIO    PRIO_MID /* Virtual I/O Priority */
#define RK_THD_PRIO PRIO_LOW /* Rumpkernel thread priority */

#define IO_BOUND_VM  1  /* VM that is I/O Bound */
#define CPU_BOUND_VM 20 /* VM that is CPU Bound: Set to a val beyond number of VMs, so no CPU BOUND VM */ 

#define VIO_BUDGET_MAX (VM_TIMESLICE) /* Maximum a I/O Tcap in DOM0 should have */
#define VIO_BUDGET_THR (VM_MIN_TIMESLICE) /* minimum excess budget with I/O tcap to transfer/delegate back to VM */

#define VIO_BUDGET_APPROX (VM_MIN_TIMESLICE * 10) /* Hack: budget transfered from I/O Tcap to DOM0's Tcap */

tcap_t vio_tcap[COS_VIRT_MACH_COUNT - 1]; /* tcap array for dom0 */
arcvcap_t vio_rcv[COS_VIRT_MACH_COUNT - 1];
tcap_res_t vio_prio[COS_VIRT_MACH_COUNT - 1];

/*
 * deficit accounting - by number of packets sent or received.
 * vio_deficit[i][j] - deficit in vmio-i for a packet processed for vmio-j
 */
unsigned int vio_deficit[COS_VIRT_MACH_COUNT - 1][COS_VIRT_MACH_COUNT - 1];
/*
 * dom0_vio_deficit[i] - if both tcaps ran out of budget, dom0 transfers budget
 *                     to not stall the processing. so deficit in dom0 for a
 *                     packet processed for vm-i.
 */
unsigned int dom0_vio_deficit[COS_VIRT_MACH_COUNT - 1]; 

#undef GRAPHTP

#ifdef GRAPHTP

#define WK1 300 //usecs
#define WK2 200 //usecs

enum vm_credits {
	DOM0_CREDITS = 1,
	DOM0_PERIOD  = 10,
	VM1_CREDITS  = 4,
	VM1_PERIOD   = 10,
	VM2_CREDITS  = 1,
	VM2_PERIOD   = 10,
};

#else

#define WK1 2000 //usecs
#define WK2 2500 //usecs
enum vm_credits {
	DOM0_CREDITS = 1,
	DOM0_PERIOD  = 10,
	VM1_CREDITS  = 4,
	VM1_PERIOD   = 10,
	VM2_CREDITS  = 6,
	VM2_PERIOD   = 10,
};

#endif


enum vm_status {
	VM_RUNNING = 0,
	VM_BLOCKED = 1,
	VM_EXPENDED = 2,
	VM_EXITED = 3,
};


enum {
	VM_CAPTBL_SELF_EXITTHD_BASE    = BOOT_CAPTBL_FREE,
	VM_CAPTBL_SELF_VKASND_BASE     = round_up_to_pow2(VM_CAPTBL_SELF_EXITTHD_BASE + CAP16B_IDSZ, CAPMAX_ENTRY_SZ), 
	VM_CAPTBL_SELF_IOTHD_BASE      = round_up_to_pow2(VM_CAPTBL_SELF_VKASND_BASE + CAP64B_IDSZ, CAPMAX_ENTRY_SZ), 
	VM_CAPTBL_SELF_IORCV_BASE      = round_up_to_pow2(VM_CAPTBL_SELF_IOTHD_BASE + CAP16B_IDSZ, CAPMAX_ENTRY_SZ), 
	VM_CAPTBL_SELF_IOASND_BASE     = round_up_to_pow2(VM_CAPTBL_SELF_IORCV_BASE + CAP64B_IDSZ, CAPMAX_ENTRY_SZ),
	VM_CAPTBL_LAST_CAP             = round_up_to_pow2(VM_CAPTBL_SELF_IOASND_BASE + CAP64B_IDSZ, CAPMAX_ENTRY_SZ),
	VM_CAPTBL_FREE                 = round_up_to_pow2(VM_CAPTBL_LAST_CAP, CAPMAX_ENTRY_SZ),
};

enum {
	VM0_CAPTBL_SELF_IOTHD_SET_BASE      = VM_CAPTBL_SELF_IOTHD_BASE, 
	VM0_CAPTBL_SELF_INITASND_SET_BASE   = round_up_to_pow2(VM0_CAPTBL_SELF_IOTHD_SET_BASE + CAP16B_IDSZ*(COS_VIRT_MACH_COUNT-1), CAPMAX_ENTRY_SZ), 
	VM0_CAPTBL_SELF_IOTCAP_SET_BASE     = round_up_to_pow2(VM0_CAPTBL_SELF_INITASND_SET_BASE + CAP64B_IDSZ*(COS_VIRT_MACH_COUNT-1), CAPMAX_ENTRY_SZ), 
	VM0_CAPTBL_SELF_IORCV_SET_BASE      = round_up_to_pow2(VM0_CAPTBL_SELF_IOTCAP_SET_BASE + CAP16B_IDSZ*(COS_VIRT_MACH_COUNT-1), CAPMAX_ENTRY_SZ), 
	VM0_CAPTBL_SELF_IOASND_SET_BASE     = round_up_to_pow2(VM0_CAPTBL_SELF_IORCV_SET_BASE + CAP64B_IDSZ*(COS_VIRT_MACH_COUNT-1), CAPMAX_ENTRY_SZ),
	VM0_CAPTBL_SELF_SLATHD_BASE         = round_up_to_pow2(VM0_CAPTBL_SELF_IOASND_SET_BASE + CAP64B_IDSZ*(COS_VIRT_MACH_COUNT-1), CAPMAX_ENTRY_SZ),
	VM0_CAPTBL_SELF_SLATCAP_BASE        = round_up_to_pow2(VM0_CAPTBL_SELF_SLATHD_BASE + CAP16B_IDSZ, CAPMAX_ENTRY_SZ),
	VM0_CAPTBL_SELF_SLARCV_BASE         = round_up_to_pow2(VM0_CAPTBL_SELF_SLATCAP_BASE + CAP16B_IDSZ, CAPMAX_ENTRY_SZ),
	VM0_CAPTBL_SELF_SLASND_BASE         = round_up_to_pow2(VM0_CAPTBL_SELF_SLARCV_BASE + CAP64B_IDSZ, CAPMAX_ENTRY_SZ),
	VM0_CAPTBL_LAST_CAP                 = round_up_to_pow2(VM0_CAPTBL_SELF_SLASND_BASE + CAP64B_IDSZ, CAPMAX_ENTRY_SZ),
	VM0_CAPTBL_FREE                     = round_up_to_pow2(VM0_CAPTBL_LAST_CAP, CAPMAX_ENTRY_SZ),
};

extern unsigned int cycs_per_usec;
extern unsigned int cycs_per_msec;

extern cycles_t dom0_sla_act_cyc;

#endif /* VK_TYPES_H */
