#ifndef RK_INV_API_H
#define RK_INV_API_H

#include <sys/socket.h>

int rk_inv_op1(void);
void rk_inv_op2(int shmid); /* OP TEST */
int rk_inv_get_boot_done(void);
int rk_inv_socket(int domain, int type, int protocol);
int rk_inv_bind(int sockfd, int shdmem_id, socklen_t addrlen);
ssize_t rk_inv_recvfrom(int s, void *buff, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
ssize_t rk_inv_sendto(int sockfd, const void *buff, size_t len, int flags, const struct sockaddr *addr, socklen_t addrlen);
int rk_inv_logdata(void); /* FIXME: using ringbuff sharedmem */
int rk_map_shdmem(int shdmem_id, int app_spdid);

int rk_socketcall(int callid, unsigned long *cargs);
int rk_socketcall_init(void);

#endif /* RK_INV_API_H */
