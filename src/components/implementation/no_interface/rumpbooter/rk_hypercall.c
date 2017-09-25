#include <cos_kernel_api.h>
#include <cos_types.h>
#include <cringbuf.h>
#include <sinv_calls.h>
#include <shdmem.h>
#include <rk_inv_api.h>
#include "rumpcalls.h"
#include "vk_types.h"
#include "micro_booter.h"
#include <sys/socket.h>

int rump___sysimpl_socket30(int, int, int);
int rump___sysimpl_bind(int, const struct sockaddr *, socklen_t);
ssize_t rump___sysimpl_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t rump___sysimpl_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);

/* These syncronous invocations involve calls to and from a RumpKernel */
extern struct cringbuf *vmrb;

#define RK_MAX_BUFF_SZ 1024
static char logdata[RK_MAX_BUFF_SZ] = { '\0' };

static void
rk_logdata_intern(void)
{
	int amnt = 0, len = 0;
	int first = 1;

	while ((amnt = cringbuf_sz(vmrb))) {
		if (first) first = 0;

		if (amnt >= RK_MAX_BUFF_SZ) amnt = RK_MAX_BUFF_SZ - 1;

		memset(logdata, '\0', RK_MAX_BUFF_SZ);
		strncpy(logdata, cringbuf_active_extent(vmrb, &len, amnt), amnt);

		printc("%s",logdata);
		cringbuf_delete(vmrb, amnt);
	}

	assert(first == 0);
}

void
rump_io_fn(void *d)
{
	arcvcap_t rcv = SUB_CAPTBL_SELF_IORCV_BASE;

	assert(vmrb);

	while (1) {
		int rcvd = 0;

		cos_rcv(rcv, RCV_ALL_PENDING, &rcvd);

		rk_logdata_intern();
	}
}

int
rk_logdata(void)
{
	rk_logdata_intern();

	return 0;
}

int
test_entry(int arg1, int arg2, int arg3, int arg4)
{
        int ret = 0;

        printc("\n*** KERNEL COMPONENT ***\n \tArguments: %d, %d, %d, %d\n", arg1, arg2, arg3, arg4);
        printc("spdid: %d\n", cos_spdid_get());
        printc("*** KERNEL COMPONENT RETURNING ***\n\n");

        return ret;
}

int
test_fs(int arg1, int arg2, int arg3, int arg4)
{
        int ret = 0;

        printc("\n*** KERNEL COMPONENT ***\n \tArguments: %d, %d, %d, %d\n", arg1, arg2, arg3, arg4);
        printc("spdid: %d\n", cos_spdid_get());

        /* FS Test */
        printc("Running paws test: VM%d\n", cos_spdid_get());
//        paws_tests();

        printc("*** KERNEL COMPONENT RETURNING ***\n\n");

        return ret;

}

int
test_shdmem(int shm_id, int arg2, int arg3, int arg4)
{
	int my_id;
	vaddr_t my_page;

	/* Calling from user component into kernel component */
	assert(!cos_spdid_get());

	/* Map in shared page */
	my_id = shmem_map_invoke(shm_id);
	assert(my_id == shm_id);

	/* Get our vaddr for this shm_id */
	my_page = shmem_get_vaddr_invoke(my_id);
	assert(my_page);

	printc("Kernel Component shared mem vaddr: %p\n", (void *)my_page);
	printc("Reading from page: %c\n", *(char *)my_page);
	printc("Writing 'b' to page + 1\n");
	*((char *)my_page + 1) = 'b';

	return 0;
}

/* For applications that wish to set up shared memory with RK, the app passes a shdmem id */
int
_rk_map_shdmem(int shdmem_id, int app_spdid)
{
	int my_shdmem_id = -1;
	extern vaddr_t shdmem_addr;

	printc("RK, mapping shared memory id: %d, from application id: %d\n",
		shdmem_id, app_spdid);
	assert(shdmem_id > -1 && app_spdid > -1);

	my_shdmem_id = shmem_map_invoke(shdmem_id);
	assert(my_shdmem_id > -1);
	shdmem_addr = shmem_get_vaddr_invoke(my_shdmem_id);
	assert(shdmem_addr > 0);

	return my_shdmem_id;
}


int
rk_socket(int domain, int type, int protocol)
{ return rump___sysimpl_socket30(domain, type, protocol); }

int
rk_bind(int sockfd, int shdmem_id, socklen_t addrlen)
{
	const struct sockaddr *addr = NULL;
	shdmem_id = shmem_map_invoke(shdmem_id);
	assert(shdmem_id > -1);
	addr = (const struct sockaddr *)shmem_get_vaddr_invoke(shdmem_id);
	assert(addr);
	return rump___sysimpl_bind(sockfd, addr, addrlen);
}

ssize_t
rk_recvfrom(int s, void *buff, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
	return rump___sysimpl_recvfrom(s, buff, len, flags, from, fromlen);
}

ssize_t
rk_sendto(int sockfd, void *buff, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen)
{
	return rump___sysimpl_sendto(sockfd, buff, len, flags, addr, addrlen);
}

int
rk_inv_entry(int arg1, int arg2, int arg3, int arg4)
{
	int ret = 0;

	/* TODO Rename this dumb conevention from a function to a system call  */
	switch(arg1) {
		case RK_INV_OP1: {
			ret = test_fs(arg2, arg3, arg4, 0);
			break;
		}
		case RK_INV_OP2: {
			ret = test_shdmem(arg2, arg3, arg4, 0);
			break;
		}
		case RK_GET_BOOT_DONE: {
			assert(0);
			break;
		}
		case RK_SOCKET: {
			ret = rk_socket(arg2, arg3, arg4);
			break;
		}
		case RK_BIND: {
			ret = rk_bind(arg2, arg3, (socklen_t)arg4);
			break;
		}
		case RK_RECVFROM: {
			int s, flags;
			void *buff;
			struct sockaddr *from;
			socklen_t *fromlen;
			size_t len;
			extern vaddr_t shdmem_addr;

			assert(shdmem_addr > 0);

			s       = arg2;
			buff    = (void *)shdmem_addr;
			len     = arg3;
			flags   = arg4;
			fromlen = (socklen_t *)(shdmem_addr + 16 + 1); /* 16 is MSG_SZ, see udpserv.c */
			from    = (struct sockaddr *)(shdmem_addr + 16 + 1 + sizeof(socklen_t));

			ret = (int)rk_recvfrom(s, buff, len, flags, from, fromlen);
			break;
		}
		case RK_SENDTO: {
			int sockfd, flags;
			void *buff;
			size_t len;
			socklen_t addrlen;
			const struct sockaddr *addr;
			extern vaddr_t shdmem_addr;

			assert(shdmem_addr > 0);

			sockfd  = (arg2 >> 16);
			buff    = (void *)shdmem_addr;
			len     = (arg2 << 16) >> 16;
			flags   = (arg3 << 16);
			addr    = (struct sockaddr *)(shdmem_addr + 16 + 1 + sizeof(socklen_t) + sizeof(struct sockaddr));
			addrlen = (arg3 << 16) >> 16;

			ret = (int)rk_sendto(sockfd, buff, len, flags, addr, addrlen);
			break;
		}
		case RK_LOGDATA: {
			ret = rk_logdata();
			break;
		}
		case RK_MAP_SHDMEM: {
			ret = _rk_map_shdmem(arg2, arg3);
			break;
		}
		default: assert(0);
	}

	return ret;
}
