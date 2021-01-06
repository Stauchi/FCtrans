#include "smemv.h"

#ifdef SMEMV
#include <stdlib.h>
#include <arpa/inet.h>
#include "rp.h"

#define CHUNK_SIZE (CHUNK_PAGES * TARGET_PAGE_SIZE)

#define FIRST_HALF
#undef LATTER_HALF

void split_chunk_lru8(unsigned char *history,
                      unsigned long total_pages, unsigned long main_pages,
                      unsigned long *sub_pages, int nr_subhosts)
{
    unsigned long histgram[256];
    unsigned short *max_hists;
    unsigned short max_history;
    int i, index;
    unsigned long pfn;
    unsigned long sum = 0;
    long left = 0;
    unsigned int host_id;
    unsigned long addr;
    
	unsigned long main = 0;
	unsigned long sub = 0;
	
	max_hists = (unsigned short *)malloc(total_pages / CHUNK_PAGES *
                                         sizeof(unsigned short));
    
    for (i = 0; i < 256; i++)
        histgram[i] = 0;

    /* create a histgram of access history (0-255) */
    for (pfn = 0; pfn < total_pages; pfn += CHUNK_PAGES) {
        max_history = 0;
        
        for (i = 0; i < CHUNK_PAGES; i++)
            max_history |= history[pfn + i];

        max_hists[pfn / CHUNK_PAGES] = max_history;
        
        index = max_history;
        histgram[index]++;
    }

    /* find a threshold (index) for sending to the main host */
    for (index = 255; index >= 0; index--) {
        sum += histgram[index] * CHUNK_PAGES;
        /* split the pages of the threshold */
        if (sum > main_pages) {
            left = main_pages - (sum - histgram[index] * CHUNK_PAGES);
            break;
        }
    }

	printf("index = %d\n", index);

    /* find a sub-host's id */
    host_id = rp_get_host_id(rp_dst, subhosts[0]);

#ifdef LATTER_HALF
    for (pfn = (total_pages - CHUNK_PAGES); pfn >= 0; pfn -= CHUNK_PAGES) {
        addr = pfn * 4096;
        max_history = max_hists[pfn / CHUNK_PAGES];

        if (max_history > index) {
            /* send to the main host over the threshold */
            for (i = 0; i < CHUNK_PAGES; i++)
                rp_insert(rp_dst, addr + i * 4096, RP_HID_MAIN);
        	main += CHUNK_PAGES;
		}
        else if (max_history < index) {
            /* send to the sub-host under the threshold */
            for (i = 0; i < CHUNK_PAGES; i++)
                rp_insert(rp_dst, addr + i * 4096, host_id);
        	sub += CHUNK_PAGES;
		}
        else {
            if (left > 0) {
                /* send some pages of the threshold to the main host */
                for (i = 0; i < CHUNK_PAGES; i++)
                    rp_insert(rp_dst, addr + i * 4096, RP_HID_MAIN);
                left -= CHUNK_PAGES;
            }
            else {
                /* send the others to the sub-host */
                for (i = 0; i < CHUNK_PAGES; i++)
                    rp_insert(rp_dst, addr + i * 4096, host_id);
            }
        }

		if(pfn == 0)
			break;
    }

	printf("test\n");
#endif

#ifdef FIRST_HALF
    for (pfn = 0; pfn < total_pages; pfn += CHUNK_PAGES) {
        addr = pfn * 4096;
        max_history = max_hists[pfn / CHUNK_PAGES];

        if (max_history > index) {
            /* send to the main host over the threshold */
            for (i = 0; i < CHUNK_PAGES; i++)
                rp_insert(rp_dst, addr + i * 4096, RP_HID_MAIN);
        	main += CHUNK_PAGES;
		}
        else if (max_history < index) {
            /* send to the sub-host under the threshold */
            for (i = 0; i < CHUNK_PAGES; i++)
                rp_insert(rp_dst, addr + i * 4096, host_id);
        	sub += CHUNK_PAGES;
		}
        else {
            if (left > 0) {
                /* send some pages of the threshold to the main host */
                for (i = 0; i < CHUNK_PAGES; i++)
                    rp_insert(rp_dst, addr + i * 4096, RP_HID_MAIN);
                left -= CHUNK_PAGES;
            }
            else {
                /* send the others to the sub-host */
                for (i = 0; i < CHUNK_PAGES; i++)
                    rp_insert(rp_dst, addr + i * 4096, host_id);
            }
        }
    }
#endif



	printf("To main: %lu, and To sub: %lu\n", main, sub);
    free(max_hists);
}
#endif /* SMEMV */
