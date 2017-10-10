#include "vk_types.h"
#include <rk_inv_api.h>
#include <cos_types.h>
#include <cos_kernel_api.h>
#include <cos_defkernel_api.h>
#include <posix.h>
#include "rumpcalls.h"
#include <sys/socket.h>

extern char *dl_str;

int rump___sysimpl_socket30(int, int, int);
int rump___sysimpl_bind(int, const struct sockaddr *, socklen_t);
ssize_t rump___sysimpl_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t rump___sysimpl_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);

static void
rk_fault_intern(void)
{
	static int spatial = 1;
	static cycles_t prev = 0, now = 0, total = 0;
	cycles_t thresh = (cycles_t)cycs_per_usec * (cycles_t)3000000;

	rdtscll(now);
	if (prev == 0) {
		prev = now;
	} else {
		total += (now - prev);
		prev = now;
	}

	if (total >= thresh) {
		if (spatial) {
			printc("INDUCING SPATIAL FAULT\n");
			dl_str = NULL;
			spatial = 0;
			total = 0;
		} else {
			printc("INDUCING TEMPORAL FAULT (spin-forever)\n");
			while(1) ;
		}
	}
}

int
rk_inv_op1(void)
{
	assert(0);
	return 0;
}

void
rk_inv_op2(int shmid)
{
	assert(0);
}

int
rk_inv_get_boot_done(void)
{
	assert(0);
	return 0;
}

int
rk_inv_socket(int domain, int type, int protocol)
{
	assert(0);
	return 0;
}
int
rk_inv_bind(int sockfd, int shdmem_id, socklen_t addrlen)
{
	assert(0);
	return 0;
}

ssize_t
rk_inv_recvfrom(int s, int buff_shdmem_id, size_t len, int flags, int from_shdmem_id, int fromlenaddr_shdmem_id)
{
	assert(0);
	return 0;
}

int
rk_inv_logdata(void)
{
	printc("%s", dl_str);
#if defined(FAULT_TEST)
	rk_fault_intern();
#endif
	return 0;
}

int
rk_socketcall(int call, unsigned long *args)
{
        int ret = -1;

        switch (call) {
		case 1: { /* Socket */
			ret = rump___sysimpl_socket30((int)*args, (int)*(args+1), (int)*(args+2));
                        break;
                }
                case 2: { /* Bind */
			ret = rump___sysimpl_bind((int)*args, (const struct sockaddr *)*(args+1), (socklen_t)*(args+2));
                        break;
                }
		case 11: { /* sendto */
			ret = rump___sysimpl_sendto((int)*args, (const void *)*(args+1), (size_t)*(args+2), (int)*(args+3), 
						    (const struct sockaddr *)*(args+4), (socklen_t)*(args+5));
			break;
		}
		case 12: { /* Recvfrom */
			ret = rump___sysimpl_recvfrom((int)*args, (void *)*(args+1), (size_t)*(args+2), (int)*(args+3),
						      (struct sockaddr *)*(args+4), (socklen_t *)*(args+5));
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
	posix_syscall_override((cos_syscall_t)rk_socketcall, __NR_socketcall);

	return 0;
}
