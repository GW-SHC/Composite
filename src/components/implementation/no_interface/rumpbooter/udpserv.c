#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "micro_booter.h"
#include "rk_inv_api.h"
#include "timer_inv_api.h"
#include "rumpcalls.h"

#define IN_PORT  9998
#define OUT_PORT 9999
#define MSG_SZ   16
#define TP_INFO_MS (unsigned long long)(5*1000) //5secs
#define HPET_REQ_US (20*1000) //20ms
#define HPET_REQ_BUDGET_US (500) //1ms

extern int vmid;

static unsigned long long __tp_out_prev = 0, __now = 0, __hpet_req_prev = 0;
static unsigned long __msg_count = 0;
static u32_t __hpets_last_pass = 0;

static volatile u32_t *__hpets_shm_addr = (u32_t *)APP_SUB_SHM_BASE;
static u32_t __interval_count = (TP_INFO_MS * 1000)/(HPET_PERIOD_US);

static void
__get_hpet_counter(void)
{
#if defined(APP_COMM_ASYNC)
	int ret = 0;
	tcap_res_t b = HPET_REQ_BUDGET_US * cycs_per_usec;

	ret = cos_tcap_delegate(APP_CAPTBL_SELF_IOSND_BASE, BOOT_CAPTBL_SELF_INITTCAP_BASE, b, UDP_PRIO, 0);
	if (ret != -EPERM) assert(ret == 0);
#elif defined(APP_COMM_SYNC)
	*__hpets_shm_addr = (u32_t)timer_get_counter();
#else
	assert(0);
#endif
}

static int
__test_udp_server(void)
{
	int fd, fdr, shdmem_id = -1;
	extern unsigned long shdmem_addr;
	struct sockaddr_in sinput;
	int msg_size=MSG_SZ;
	int tp_counter = 0;

	PRINTC("Sending to port %d\n", OUT_PORT);
	if ((fd = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
		PRINTC("Error Establishing socket\n");
		return -1;
	}
	PRINTC("fd for socket: %d\n", fd);
	if ((fdr = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
		PRINTC("Error Establishing receive socket\n");
		return -1;
	}
	PRINTC("fd for receive socket: %d\n", fdr);

	sinput.sin_family      = AF_INET;
	sinput.sin_port        = htons(IN_PORT);
	sinput.sin_addr.s_addr = htonl(INADDR_ANY);
	PRINTC("binding receive socket to port %d\n", IN_PORT);
	if (bind(fdr, (struct sockaddr *)&sinput, sizeof(sinput))) {
		PRINTC("binding receive socket\n");
		return -1;
	}

	rdtscll(__now);
	__tp_out_prev = __now;

	/* Allocate shared memory to use for recvfrom and sendto */
	assert(shdmem_addr > 0);
	unsigned long shdmem_max = shdmem_addr + 4096;

	/* Set up buffer */
	PRINTC("udpserver, buffer addr: %p\n", (void *)shdmem_addr);
	char *__msg = (char *)shdmem_addr;
	*__msg = '\0';
	shdmem_addr += MSG_SZ + 1;
	assert(shdmem_addr < shdmem_max);

	/* Set up from addr len */
	PRINTC("udpserver, len addr: %p\n", (void *)shdmem_addr);
	socklen_t *len = (socklen_t *)shdmem_addr;
	*len = sizeof(struct sockaddr);
	shdmem_addr += sizeof(socklen_t);
	assert(shdmem_addr < shdmem_max);

	/* Set up from addr struct */
	PRINTC("udpserver, sa addr: %p\n", (void *)shdmem_addr);
	struct sockaddr *sa = (struct sockaddr *)shdmem_addr;
	shdmem_addr += sizeof(struct sockaddr);
	assert(shdmem_addr < shdmem_max);

	/* Set up output addr struct */
	PRINTC("udpserver, soutput addr: %p\n", (void *)shdmem_addr);
	struct sockaddr_in *soutput = (struct sockaddr_in *)shdmem_addr;
	shdmem_addr += sizeof(struct sockaddr_in);
	assert(shdmem_addr < shdmem_max);

	soutput->sin_family      = AF_INET;
	soutput->sin_port        = htons(OUT_PORT);
	soutput->sin_addr.s_addr = htonl(INADDR_ANY);

	do {

		if (recvfrom(fdr, __msg, msg_size, 0, sa, len) != msg_size) {
			PRINTC("read ERROR\n");
			continue;
		}
		//PRINTC("Received-msg: seqno:%u time:%llu\n", ((unsigned int *)__msg)[0], ((unsigned long long *)__msg)[1]);
		/* Reply to the sender */
		/* TODO soutput needs to be in shared memory */
		soutput->sin_addr.s_addr = ((struct sockaddr_in*)sa)->sin_addr.s_addr;
		if (sendto(fd, __msg, msg_size, 0, (struct sockaddr*)soutput, sizeof(struct sockaddr_in)) < 0) {
			PRINTC("sendto ERROR\n");
			continue;
		}

		//PRINTC("Sent-msg: seqno:%u time:%llu\n", ((unsigned int *)__msg)[0], ((unsigned long long *)__msg)[1]);

		__msg_count++;
		rdtscll(__now);

		/* Request Number of HPET intervals passed. Every HPET_REQ_US usecs */
		if ((__now - __hpet_req_prev) >= ((cycles_t)cycs_per_usec*HPET_REQ_US)) {
			__hpet_req_prev = __now;
			__get_hpet_counter();
		}

		/* Log every __interval_count number of HPETS processed */
		if (*__hpets_shm_addr >= __hpets_last_pass + __interval_count) {
			PRINTC("%d:Msgs processed:%lu, last seqno:%u\n", tp_counter++, __msg_count, ((unsigned int *)__msg)[0]);
			__msg_count = 0;
			__tp_out_prev = __now;
			__hpets_last_pass = *__hpets_shm_addr;
		}

	} while (1) ;

	return -1;
}

int
udpserv_main(void)
{
	rk_socketcall_init();

	PRINTC("Starting udp-server [in:%d out:%d]\n", IN_PORT, OUT_PORT);
	__test_udp_server();

	return 0;
}
