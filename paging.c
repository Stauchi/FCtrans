#include "smemv.h"

#ifdef SMEMV
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/sockets.h"
#include "qemu/rcu_queue.h"
#include "qemu/bitops.h"
#include "exec/ram_addr.h"
#include "qapi/error.h"
#include "rp.h"
#include "qemu/timer.h"

#define __NR_userfaultfd 323
#include "userfaultfd.h"

#define CHUNK_SIZE (CHUNK_PAGES * TARGET_PAGE_SIZE)

#undef FCtrans_log
#define FCtrans

#ifdef FCtrans
#include "migration.h"
#include "qemu/bitmap.h"
#include "qemu/timer.h"
#endif

#define FCtrans_log
extern unsigned long pagein_num;
extern unsigned long pageout_num;
#endif

static char page[4096];
static int ufd;

void *qemu_get_ram_ptr_safe(ram_addr_t addr);

#ifdef FCtrans
extern unsigned long *FCtrans_bitmap;
extern unsigned long FCtrans_bitmap_size;
extern unsigned long free_pages_in_main_host;
int guest_flag = 0;
#endif

static int read_exact(int s, char *buf, size_t size)
{
    int rcvd = 0;
    int ret;

    while (rcvd < size) {
        ret = read(s, &buf[rcvd], size - rcvd);
        if (ret < 0) {
          perror("recv");
          return -1;
        }
        else if (ret == 0)
          break;

        rcvd += ret;
    }

    return rcvd;
}

static int send_pagein_request(int mem_sock, ram_addr_t pa)
{
    unsigned int com = 2;
    int ret;

    ret = send(mem_sock, &com, sizeof(com), 0);
    if (ret == -1 || ret == 0) {
        perror("pagein: send command");
        return -1;
    }

    ret = send(mem_sock, &pa, sizeof(pa), 0);
    if(ret == -1 || ret == 0) {
        perror("pagein: send addr");
        return -1;
    }

    return 0;
}

static int recv_pagein_response(int mem_sock)
{
    struct uffdio_copy copy_struct;
    ram_addr_t pa;
    char *addr;
    unsigned long pfn;
    int ret;

    ret = read_exact(mem_sock, ( char *)&pa, sizeof(pa));
    if (ret == -1 || ret == 0) {
        perror("pagein: read ram addr");
        return -1;
    }

    if (pa == (unsigned long)-1) {
        printf("pagein: no page in sub-host\n");
        return 0;  /* race condition */
    }

    ret = read_exact(mem_sock, page, TARGET_PAGE_SIZE);
    if (ret == -1 || ret == 0) {
        perror("pagein: read mem");
        return -1;
    }

    addr = qemu_get_ram_ptr_safe(pa);
    if (addr == NULL) {
        printf("pagein: no host page\n");
        return -1;
    }

    /* write contents to the accessed page */
    copy_struct.dst = (unsigned long)addr;
    copy_struct.src = (unsigned long)page;
    copy_struct.len = TARGET_PAGE_SIZE;
    copy_struct.mode = 0;
  
    if (ioctl(ufd, UFFDIO_COPY, &copy_struct)) {
        perror("pagein: uffdio_copy");
        return -1;
    }
  
    rp_insert(rp_src, pa, RP_HID_MAIN);

    /* set access history for the paged-in page */
    pfn = pa / TARGET_PAGE_SIZE;

    history[pfn] |= (1 << 7);

    return 0;
}

static int send_pageout_request(int mem_sock, ram_addr_t pa,
                                 unsigned int host_id)
{
    struct uffdio_pull pull_struct;
    char *addr;
    unsigned int com = 1;
    int ret;

    addr = qemu_get_ram_ptr_safe(pa);
    if (addr == NULL) {
        printf("pageout: no host memory\n");
        return -1;
    }

    /* remove mapped pages */
    pull_struct.dst = (unsigned long)page;
    pull_struct.src = (unsigned long)addr;
    pull_struct.len = TARGET_PAGE_SIZE;

    if (ioctl(ufd, UFFDIO_PULL, &pull_struct)) {
        perror("pageout: uffdio_pull");
        return -1;
    }

    /* send Swapout command */
    ret = send(mem_sock, &com, sizeof(com), 0);
    if (ret == -1 || ret == 0) {
        perror("pageout: send command");
        return -1;
    }

    /* Send page address to memory server */
    ret = send(mem_sock, (char *)&pa, sizeof(pa), 0);
    if (ret == -1 || ret == 0) {
        perror("pageout: send addr");
        return -1;
    }

    /* Send page data to memory server */
    ret = send(mem_sock, page, TARGET_PAGE_SIZE, 0);
    if (ret == -1 || ret == 0) {
        perror("pageout: send data");
        return -1;
    }

    rp_insert(rp_src, pa, host_id);

    return 0;
}

static int pagein_chunk(ram_addr_t pa_target)
{
    unsigned int host_id;
    int mem_sock;
    ram_addr_t pa_start, pa;
    int ret;

    host_id = rp_search(rp_src, pa_target);

    if (!rp_is_host_sub(rp_src, host_id)) {
        printf("pagein: no sub-host's memory: %lx\n", pa_target);
        return RP_HID_UNDEF;
    }

    mem_sock = rp_get_host_sock(rp_src, host_id);
    if (mem_sock == -1) {
        printf("pagein: invalid socket\n");
        return RP_HID_UNDEF;
    }

    pa_start = pa_target & ~(CHUNK_SIZE - 1);

    /* fault page first */
    ret = send_pagein_request(mem_sock, pa_target);
    if (ret == -1)
        return RP_HID_UNDEF;

    for (pa = pa_start; pa < pa_start + CHUNK_SIZE; pa += TARGET_PAGE_SIZE) {
        if (pa == pa_target)
            continue;
#ifdef FCtrans
			if(test_bit(pa / TARGET_PAGE_SIZE, FCtrans_bitmap) == 0)
				continue;
#endif
        ret = send_pagein_request(mem_sock, pa);
        if (ret == -1)
            return RP_HID_UNDEF;
    }

    for (pa = pa_start; pa < pa_start + CHUNK_SIZE; pa += TARGET_PAGE_SIZE) {
        ret = recv_pagein_response(mem_sock);
        if (ret == -1)
            return RP_HID_UNDEF;
#ifdef FCtrans
		if(test_bit(pa / TARGET_PAGE_SIZE, FCtrans_bitmap) == 0)
			continue;
#endif
   }

    pagein_num++;

    return host_id;
}

static int pageout_chunk_lru8(unsigned char *history, unsigned long nr_pages,
                              ram_addr_t pa_pagein)
{
    unsigned long pfn;
    unsigned char max_history;
    unsigned char min_histgram = 255;
    unsigned long selected_pfn = 0;
    unsigned int id;
    int i;

    /* find a least-recently used page */
    for (pfn = 0; pfn < nr_pages; pfn += CHUNK_PAGES) {
        max_history = 0;
      
        for (i = 0; i < CHUNK_PAGES; i++)
            max_history |= history[pfn + i];

        if (max_history < min_histgram) {
            id = rp_search(rp_src, pfn * 4096);

            if (!rp_is_host_main(rp_src, id))
                continue;

            if (pfn * 4096 == pa_pagein)
                continue;

            min_histgram = max_history;
            selected_pfn = pfn;

            if (min_histgram == 0)
                break;
        }
    }

    return selected_pfn;
}

static void pageout_chunk(unsigned int host_id, ram_addr_t pa_pagein)
{
    int mem_sock;
    unsigned long nr_pages;
    unsigned long pfn;
    ram_addr_t pa, pa_start;
    
    mem_sock = rp_get_host_sock(rp_src, host_id);
    if (mem_sock == -1) {
        printf("invalid socket\n");
        return;
    }

    nr_pages = rp_get_mem_size(rp_src) / TARGET_PAGE_SIZE;

    /* Select memory address in destination Main host */
    pfn = pageout_chunk_lru8(history, nr_pages, pa_pagein);

    pa_start = pfn * TARGET_PAGE_SIZE;

    for (pa = pa_start; pa < pa_start + CHUNK_SIZE; pa += TARGET_PAGE_SIZE){
#ifdef FCtrans
		if(test_bit(pa / TARGET_PAGE_SIZE, FCtrans_bitmap) == 0)
			continue;
#endif
        send_pageout_request(mem_sock, pa, host_id);
	}
	pageout_num++;
}

static void *fault_thread(void *arg)
{
    struct pollfd pfd[1];
    struct uffd_msg msg;
    char *addr;
    ram_addr_t pa;
    unsigned int host_id;
    int ret;
#ifdef FCtrans
	struct uffdio_copy copy_struct;
	unsigned long addr_0;
	unsigned long pa_0;
#endif

    while (1) {
        pfd[0].fd = ufd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;

        /* wait for an event from userfaultfd */
        if (poll(pfd, 1, -1) == -1) {
            perror("poll");
            break;
        }

        /* receive an event */
        ret = read(ufd, &msg, sizeof(msg));
        if (ret != sizeof(msg)) {
            perror("read ufd");
            break;
        }

        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            fprintf(stderr, "unexpected event\n");
            break;
        }

        addr = (void *)(msg.arg.pagefault.address & TARGET_PAGE_MASK);
#ifdef FCtrans
		addr_0 = (unsigned long)((unsigned long)addr & ~(CHUNK_SIZE -1));
#endif
        pa = qemu_ram_addr_from_host(addr);
        if (pa == RAM_ADDR_INVALID) {
            fprintf(stderr, "fault outside guest: %p\n", addr);
            continue;
        }

#ifndef FCtrans
        pa_0 = qemu_ram_addr_from_host(addr_0);
		if (pa == RAM_ADDR_INVALID) {
			fprintf(stderr, "fault1 outside guest: %p\n", addr);
			continue;
		}
#endif
#ifndef FCtrans
		if(test_bit(pa / TARGET_PAGE_SIZE, FCtrans_bitmap) == 0){
            if(find_next_bit(FCtrans_bitmap, ((pa_0 / TARGET_PAGE_SIZE) + CHUNK_PAGES), pa_0 / TARGET_PAGE_SIZE) < ((pa_0 / TARGET_PAGE_SIZE) + CHUNK_PAGES)){
                copy_struct.dst = (unsigned long)addr;
                copy_struct.src = (unsigned long)ZERO_PAGE;
                copy_struct.len = TARGET_PAGE_SIZE;
                copy_struct.mode = 0;

                if (ioctl(ufd, UFFDIO_COPY, &copy_struct)) {
                    perror("ioctl:zero copy");
                    exit(1);
                }

                bitmap_set(FCtrans_bitmap, pa / TARGET_PAGE_SIZE, 1);
                if(guest_flag == 1)
                    rp_insert(rp_src, pa ,RP_HID_MAIN);

            }else{
                for(PA = 0; PA < CHUNK_SIZE; PA+=TARGET_PAGE_SIZE){
                    copy_struct.dst = (unsigned long)addr_0 + PA;
                    copy_struct.src = (unsigned long)ZERO_PAGE;
                    copy_struct.len = TARGET_PAGE_SIZE;
                    copy_struct.mode = 0;

                    if (ioctl(ufd, UFFDIO_COPY, &copy_struct)) {
                        perror("ioctl:zero copy");
                        exit(1);
                    }

                    bitmap_set(FCtrans_bitmap, (pa_0 + PA) / TARGET_PAGE_SIZE, 1);
                    if(guest_flag == 1)
                        rp_insert(rp_src, pa_0 + PA,RP_HID_MAIN);
                }

            }
        }else{
			host_id = pagein_chunk(pa);

        	if (host_id == RP_HID_UNDEF || host_id == RP_HID_MAIN)
            	continue;

			free_pages_in_main_host -= CHUNK_PAGES;

			while(free_pages_in_main_host < CHUNK_PAGES){
				pageout_chunk(host_id, pa & ~(CHUNK_SIZE - 1));
				free_pages_in_main_host += CHUNK_PAGES;
			}
		}
    }

    return NULL;
}

void setup_paging(void)
{
    RAMBlock *block;
    struct uffdio_api api_struct;
    struct uffdio_register reg_struct;
    unsigned int host_id;
    struct in_addr addr;
    char host_port[64];
    int mem_sock;
    QemuThread t;
    
    /* search the first sub-host (after main host) */
    host_id = rp_get_next_host(rp_src, RP_HID_MAIN);

    while (rp_is_host_sub(rp_src, host_id)) {
        /* connect to a sub-host */
        addr.s_addr = rp_get_host_addr(rp_src, host_id);
        sprintf(host_port, "%s:9737", inet_ntoa(addr));

        mem_sock = inet_connect(host_port, NULL);
        if (mem_sock < 0)
            perror("qemu_loadvm_state: inet_connect");

        /* register "host_id -> mem_sock" */
        rp_set_host_sock(rp_src, host_id, mem_sock);

        /* search the next sub-host */
        host_id = rp_get_next_host(rp_src, host_id);
    }

    /* check userfaultfd */
    ufd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (ufd == -1) {
        perror("userfaultfd");
        exit(1);
    }

    api_struct.api = UFFD_API;
    api_struct.features = 0;

    if (ioctl(ufd, UFFDIO_API, &api_struct)) {
        perror("ioctl: UFFD_API");
        exit(1);
    }

    qemu_thread_create(&t, "userfaultfd", fault_thread, NULL,
                       QEMU_THREAD_JOINABLE);

    QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        reg_struct.range.start = (unsigned long)block->host;
        reg_struct.range.len = block->max_length;
        reg_struct.mode = UFFDIO_REGISTER_MODE_MISSING;

        if (ioctl(ufd, UFFDIO_REGISTER, &reg_struct)) {
            perror("ioctl: UFFDIO_REGISTER");
            exit(1);
        }
    }
#ifdef FCtrans
	guest_flag = 1;
#endif
}
#endif /* SMEMV */
