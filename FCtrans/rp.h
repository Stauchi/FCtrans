#ifndef __RP_H_
#define __RP_H_

struct rp;

/* special host ids */
#define RP_HID_MAIN 0
#define RP_HID_UNDEF 255

struct rp *rp_init(unsigned long mem_size);
void rp_free(struct rp *rp);
int rp_insert(struct rp *rp, unsigned long addr, unsigned int host_id);
unsigned int rp_search(struct rp *rp, unsigned long addr);

unsigned int rp_get_host_id(struct rp *rp, in_addr_t host);
in_addr_t rp_get_host_addr(struct rp *rp, unsigned int host_id);
unsigned int rp_get_next_host(struct rp *rp, unsigned int host_id);

int rp_is_host_main(struct rp *rp, unsigned int host_id);
int rp_is_host_sub(struct rp *rp, unsigned int host_id);
int rp_is_host_undef(struct rp *rp, unsigned int host_id);

int rp_set_host_sock(struct rp *rp, unsigned int host_id, int sock);
int rp_get_host_sock(struct rp *rp, unsigned int host_id);

unsigned long rp_get_mem_size(struct rp *rp);

#endif /* __RP_H_ */

