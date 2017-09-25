#include "vk_types.h"
#include <rk_inv_api.h>
#include <cos_types.h>
#include <cos_kernel_api.h>
#include <cos_defkernel_api.h>
#include <posix.h>
#include "rumpcalls.h"

extern int vmid;

int
rk_inv_op1(void) {
	return cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_INV_OP1, 0, 0, 0);
}

void
rk_inv_op2(int shmid)
{
	cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_INV_OP2, shmid, 0, 0);
}

int
rk_inv_get_boot_done(void)
{
	return cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_GET_BOOT_DONE, 0, 0, 0);
}

int
rk_inv_socket(int domain, int type, int protocol)
{
	return cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_SOCKET, domain, type, protocol);
}

int
rk_inv_bind(int sockfd, int shdmem_id, socklen_t addrlen)
{
	return cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_BIND, sockfd, shdmem_id, addrlen);
}

ssize_t
rk_inv_recvfrom(int s, void *buff, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
	assert(s <= 0xFFFF);
	assert(len <= 0xFFFF);
	assert(flags <= 0xFFFF);
	/* buff, from and fromlen are stored in page and will be pointed to on RK side */

	return (ssize_t)cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_RECVFROM,
			s, len, flags);
}

ssize_t
rk_inv_sendto(int sockfd, const void *buff, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen)
{
	assert(sockfd <= 0xFFFF);
	assert(len <= 0xFFFF);
	assert(flags <= 0xFFFF);
	assert(addrlen <= (socklen_t)0xFFFF);
	/* buff and addr are stored in page and will be pointed to on RK side */

	return (ssize_t)cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_SENDTO,
			((unsigned long)sockfd << (unsigned long)16) | (unsigned long)len,
			((unsigned long)flags << (unsigned long)16) | (unsigned long)addrlen,
			0);
}

/* still using ringbuffer shared data */
int
rk_inv_logdata(void)
{
	return cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_LOGDATA, 0, 0, 0);
}

int
rk_socketcall(int call, unsigned long *args)
{
        int ret = -1;
	/*
	 * Set and unset by sendto and recvfrom to ensure that only 1 thread
	 * is sending and recieving packets. This means that we will never have
	 * have more than 1 packet in the send or recv shdmem page at a given time
	 */
	static int canSend = 0;

        switch (call) {
		case 1: { /* Socket */
                        int domain, type, protocol;

                        domain     = *args;
                        type       = *(args + 1);
                        protocol   = *(args + 2);
                        ret = rk_inv_socket(domain, type, protocol);

                        break;
                }
                case 2: { /* Bind */
                        int sockfd, shdmem_id;
                        vaddr_t shdmem_addr;
                        void *addr;
                        u32_t addrlen;

                        sockfd  = *args;
                        addr    = (void *)*(args + 1);
                        addrlen = *(args + 2);

                        /*
                         * Do stupid shared memory for now
                         * allocate a page for each bind addr
                         * don't deallocate. #memLeaksEverywhere
			 * We don't have to fix this for now as we only have 1 bind
                         */

			/* TODO make this a function */
                        shdmem_id = shmem_allocate_invoke();
                        assert(shdmem_id > -1);
                        shdmem_addr = shmem_get_vaddr_invoke(shdmem_id);
                        assert(shdmem_addr > 0);

                        memcpy((void * __restrict__)shdmem_addr, addr, addrlen);
                        ret = rk_inv_bind(sockfd, shdmem_id, addrlen);

                        break;
                }
		case 11: { /* sendto */
			int fd, flags;
			static int shdmem_id = -1;
			static vaddr_t shdmem_addr = 0;
			vaddr_t shdmem_addr_tmp;
			const void *buff;
			void *shdmem_buff;
			size_t len;
			const struct sockaddr *addr;
			struct sockaddr *shdmem_sockaddr;
			socklen_t addrlen;

			fd      = (int)*args;
			buff    = (const void *)*(args + 1);
			len     = (size_t)*(args + 2);
			flags   = (int)*(args + 3);
			addr    = (const struct sockaddr *)*(args + 4);
			addrlen = (socklen_t)*(args + 5);

			assert(canSend == 1);
			canSend = 0;

			ret = (int)rk_inv_sendto(fd, buff, len, flags,
				addr, addrlen);

			break;
		}
		case 12: { /* Recvfrom */
                        int s, flags;
			static int shdmem_id = -1;
			static vaddr_t shdmem_addr = 0;
			vaddr_t shdmem_addr_tmp;
                        void *buff;
                        size_t len;
                        struct sockaddr *from_addr;
                        u32_t *from_addr_len_ptr;

                        s                 = *args;
                        buff              = (void *)*(args + 1);
                        len               = *(args + 2);
                        flags             = *(args + 3);
                        from_addr         = (struct sockaddr *)*(args + 4);
                        from_addr_len_ptr = (u32_t *)*(args + 5);

			assert(canSend == 0);
			canSend = 1;

                        ret = (int)rk_inv_recvfrom(s, buff, len, flags,
                                from_addr, from_addr_len_ptr);

                        break;
                }
                default:
                        assert(0);
        }

        return ret;
}

int
rk_socketcall_init(void)
{
	assert(vmid != 0);

	posix_syscall_override((cos_syscall_t)rk_socketcall, __NR_socketcall);

	return 0;
}

int
rk_map_shdmem(int shdmem_id, int app_spdid)
{
	return cos_sinv(APP_CAPTBL_SELF_RK_SINV_BASE, RK_MAP_SHDMEM, shdmem_id, app_spdid, 0);
}
