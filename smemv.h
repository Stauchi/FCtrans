#ifndef __SMEMV_H
#define __SMEMV_H

#define SMEMV

#define CHUNK_PAGES 512  /* 2^9 pages = 2 MB*/

#include <arpa/inet.h>

struct rp;

extern struct rp *rp_src, *rp_dst;
extern unsigned char *history;
extern in_addr_t subhosts[];
extern int nr_subhosts;
extern unsigned long vm_mem_size;
extern unsigned long subhost_bytes;

void get_vm_mem_size(void);
void setup_paging(void);
void split_chunk_lru8(unsigned char *history,
                      unsigned long total_pages, unsigned long main_pages,
                      unsigned long *sub_pages, int nr_subhosts);

#endif /* __SMEMV_H */
