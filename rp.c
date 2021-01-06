#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "rp.h"

#define PAGE_SIZE 4096

#define MAX_HOST 255

#define RP_HOST_MAIN INADDR_LOOPBACK
#define RP_HOST_UNDEF INADDR_NONE

struct rp {
    in_addr_t hosts[MAX_HOST];  /* host id -> addr */
    int sock[MAX_HOST];  /* socket for memory server */
    unsigned char *mem_loc;  /* host id for each memory page */
    unsigned long nr_pfns;  /* # of pages */
    QemuMutex lock;  /* lock for hosts */
};

/* called at first */
struct rp *rp_init(unsigned long mem_size)
{
    struct rp *rp;
    int i;

    rp = malloc(sizeof(struct rp));
    if (rp == NULL) {
        printf("rp_init: cannot allocate rp\n");
        return NULL;
    }

    rp->nr_pfns = mem_size / PAGE_SIZE;

    rp->mem_loc = (unsigned char *)malloc(rp->nr_pfns);
    if (rp->mem_loc == NULL) {
        printf("rp_init: cannot allocate mem_loc\n");
        free(rp);
        return NULL;
    }

    /* initialized by RP_HID_UNDEF */
    memset(rp->mem_loc, RP_HID_UNDEF, rp->nr_pfns);

    rp->hosts[RP_HID_MAIN] = RP_HOST_MAIN;

    for (i = 1; i < MAX_HOST; i++)
        rp->hosts[i] = RP_HOST_UNDEF;

    for (i = 1; i < MAX_HOST; i++)
        rp->sock[i] = -1;

    qemu_mutex_init(&rp->lock);

    return rp;
}

/* close all connection to sub-hosts */
void rp_free(struct rp *rp)
{
    int i;

    if (rp == NULL) {
        printf("rp_free: rp is null\n");
        return;
    }

    for (i = 1; i < MAX_HOST; i++) {
        if (rp->sock[i] != -1)
            close(rp->sock[i]);
    }

    free(rp);
}

/* register "addr -> host id" */
int rp_insert(struct rp *rp, unsigned long addr, unsigned int host_id)
{
    unsigned long pfn;

    if (rp == NULL) {
        printf("rp_insert: rp is null\n");
        return -1;
    }

    pfn = addr / PAGE_SIZE;
    if (pfn >= rp->nr_pfns) {
        printf("rp_insert: too large address: %lx\n", addr);
        return -1;
    }

    if (host_id >= MAX_HOST) {
        printf("rp_insert: invalid host id: %u\n", host_id);
        return -1;
    }

    rp->mem_loc[pfn] = host_id;

    return 0;
}

/* addr -> host id */
unsigned int rp_search(struct rp *rp, unsigned long addr)
{
    unsigned long pfn;

    if (rp == NULL) {
        printf("rp_search: rp is null\n");
        return RP_HID_UNDEF;
    }

    pfn = addr / PAGE_SIZE;
    if (pfn >= rp->nr_pfns) {
        printf("rp_search: too large address: %lx\n", addr);
        return RP_HID_UNDEF;
    }

    return rp->mem_loc[pfn];
}

/* host addr -> host id */
// XXX should use hash
unsigned int rp_get_host_id(struct rp *rp, in_addr_t host)
{
    int id;

    if (rp == NULL) {
        printf("rp_get_host_id: rp is null\n");
        return RP_HID_UNDEF;
    }

    /* need atomic host insertion */
    qemu_mutex_lock(&rp->lock);

    /* search a host id */
    for (id = 0; id < MAX_HOST; id++) {
        if (rp->hosts[id] == host) {
            qemu_mutex_unlock(&rp->lock);
            return id;
        }
    }

    /* add a host and allocate a new id */
    for (id = 1; id < MAX_HOST; id++) {
        if (rp->hosts[id] == RP_HOST_UNDEF) {
            rp->hosts[id] = host;
            qemu_mutex_unlock(&rp->lock);
            return id;
        }
    }

    qemu_mutex_unlock(&rp->lock);

    printf("rp_get_host_id: too many hosts\n");

    return RP_HID_UNDEF;  /* full */
}

/* host id -> host addr */
in_addr_t rp_get_host_addr(struct rp *rp, unsigned int host_id)
{
    if (rp == NULL) {
        printf("rp_get_host_addr: rp is null\n");
        return RP_HOST_UNDEF;
    }

    if (host_id >= MAX_HOST)
        return RP_HOST_UNDEF;

    return rp->hosts[host_id];
}

/* find the next sub-host with host id greater than host_id */
unsigned int rp_get_next_host(struct rp *rp, unsigned int host_id)
{
    int id;

    if (rp == NULL) {
        printf("rp_get_next_host: rp is null\n");
        return RP_HID_UNDEF;
    }

    for (id = host_id + 1; id < MAX_HOST; id++) {
        if (rp_is_host_sub(rp, id))
            return id;
    }

    return RP_HID_UNDEF;
}

/* return 1 if the main host */
int rp_is_host_main(struct rp *rp, unsigned int host_id)
{
    return host_id == RP_HID_MAIN;
}

/* return 1 if a sub-host */
int rp_is_host_sub(struct rp *rp, unsigned int host_id)
{
    if (rp == NULL) {
        printf("rp_is_host_sub: rp is null\n");
        return 0;
    }

    if (host_id == RP_HID_UNDEF || host_id > MAX_HOST)
        return 0;

    if (rp->hosts[host_id] == RP_HOST_UNDEF)
        return 0;

    return host_id != RP_HID_MAIN;
}

/* return 1 if no main or sub host */
int rp_is_host_undef(struct rp *rp, unsigned int host_id)
{
    if (rp == NULL) {
        printf("rp_is_host_undef: rp is null\n");
        return 1;
    }

    if (host_id == RP_HID_UNDEF || host_id > MAX_HOST)
        return 1;

    return rp->hosts[host_id] == RP_HOST_UNDEF;
}

/* set socket for a sub-host with host_id */
int rp_set_host_sock(struct rp *rp, unsigned int host_id, int sock)
{
    if (rp == NULL) {
        printf("rp_set_host_sock: rp is null\n");
        return -1;
    }

    if (host_id >= MAX_HOST) {
        printf("rp_set_host_sock: invalid host id: %u\n", host_id);
        return -1;
    }

    rp->sock[host_id] = sock;

    return 0;
}

/* return socket for a sub-host with host_id */
int rp_get_host_sock(struct rp *rp, unsigned int host_id)
{
    if (rp == NULL) {
        printf("rp_get_host_sock: rp is null\n");
        return -1;
    }

    if (host_id >= MAX_HOST) {
        printf("rp_get_host_sock: invalid host id: %u\n", host_id);
        return -1;
    }

    return rp->sock[host_id];
}

/* return memory size specified in rp_init() */
unsigned long rp_get_mem_size(struct rp *rp)
{
    if (rp == NULL) {
        printf("rp_get_mem_size: rp is null\n");
        return 0;
    }

    return rp->nr_pfns * PAGE_SIZE;
}
