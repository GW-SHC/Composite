#include <stdio.h>
#include <string.h>

#include <cos_component.h>
#include <cobj_format.h>
#include <cos_kernel_api.h>
#include <boot_deps.h>

struct cos_compinfo boot_info;	
//struct cos_compinfo comp_info[MAX_NUM_SPDS+1];	

struct cobj_header *hs[MAX_NUM_SPDS+1];

extern vaddr_t cos_upcall_entry;

static void
cos_llprint(char *s, int len)
{ call_cap(PRINT_CAP_TEMP, (int)s, len, 0, 0); }


int
prints(char *s)
{
		int len = strlen(s);

			cos_llprint(s, len);

				return len;
}

int __attribute__((format(printf,1,2)))
printc(char *fmt, ...)
{
	  char s[128];
  	  va_list arg_ptr;
  	  int ret, len = 128;

  	  va_start(arg_ptr, fmt);
  	  ret = vsnprintf(s, len, fmt, arg_ptr);
  	  va_end(arg_ptr);
	  cos_llprint(s, ret);

	  return ret;
}


struct deps {
	short int client, server;
};
struct deps *deps;
int ndeps;

/*Component init info*/

#define INIT_STR_SZ 52

struct component_init_str {
        unsigned int spdid, schedid;
        int startup;
        char init_str[INIT_STR_SZ];
}__attribute__((packed));

struct component_init_str *init_args;

unsigned int *boot_sched;

static void
boot_find_cobjs(struct cobj_header *h, int n)
{
	int i;
	vaddr_t start, end;

	start = (vaddr_t)h;
	hs[0] = h;

	//this walks hs[] through the list of objects
	//becasue we only have one object, it's already set to right addr
	for (i = 1 ; i < n ; i++) {
		int j = 0, size = 0, tot = 0;

		size = h->size;
		for (j = 0 ; j < (int)h->nsect ; j++) {
	//		printc("\tsection %d, size %d\n", j, cobj_sect_size(h, j));
			tot += cobj_sect_size(h, j);
		}
		printc("cobj %s:%d found at %p:%x, size %x -> %x\n",
		       h->name, h->id, hs[i-1], size, tot, cobj_sect_get(hs[i-1], 0)->vaddr);

		end   = start + round_up_to_cacheline(size);
		hs[i] = h = (struct cobj_header*)end;
		start = end;
	}
	
	//set the hs[n] to null so we can loop through easily later
	hs[n] = NULL;
	printc("cobj %s:%d found at %p -> %x\n",
	       hs[n-1]->name, hs[n-1]->id, hs[n-1], cobj_sect_get(hs[n-1], 0)->vaddr);
}

static vaddr_t
boot_spd_end(struct cobj_header *h)
{
	struct cobj_sect *sect;
	int max_sect;

	max_sect = h->nsect-1;
	sect     = cobj_sect_get(h, max_sect);

	return sect->vaddr + round_up_to_page(sect->bytes);
}

static int
boot_spd_symbs(struct cobj_header *h, spdid_t spdid, vaddr_t *comp_info)
{
	printc("boot_spd_symbs: %d \n", h->nsymb);
	int i = 0;
	for(i = 0; i < h->nsymb; i++) {
		struct cobj_symb *symb;

		symb = cobj_symb_get(h, i);
		assert(symb);
		if (COBJ_SYMB_UNDEF == symb->type) break;

		switch (symb->type) {
		case COBJ_SYMB_COMP_INFO:
			printc("FOUND IT! \n");
			*comp_info = symb->vaddr;
			break;
		default:
			printc("boot: Unknown symbol type %d\n", symb->type);
			break;
		}

	}
	return 1;
}

static int
boot_process_cinfo(struct cobj_header *h, spdid_t spdid, vaddr_t heap_val,
		   char *mem, vaddr_t symb_addr)
{
	int i;
	struct cos_component_information *ci;

	assert(symb_addr == round_to_page(symb_addr));
	ci = (struct cos_component_information*)(mem);

	if (!ci->cos_heap_ptr) ci->cos_heap_ptr = heap_val;
	ci->cos_this_spd_id = spdid;
	ci->init_string[0]  = '\0';
	for (i = 0 ; init_args[i].spdid ; i++) {
		char *start, *end;
		int len;

		if (init_args[i].spdid != spdid) continue;

		start = strchr(init_args[i].init_str, '\'');
		if (!start) break;
		start++;
		end   = strchr(start, '\'');
		if (!end) break;
		len   = (int)(end-start);
		memcpy(&ci->init_string[0], start, len);
		ci->init_string[len] = '\0';
	}

	/* save the address of this page for later retrieval
	 * (e.g. to manipulate the stack pointer) */
	/* comp_info_record(h, spdid, ci); */

	return 1;
}

static int
boot_comp_map_memory(struct cobj_header *h, spdid_t spdid, vaddr_t comp_info)
{
	int i;
	int flag;
	vaddr_t dest_daddr, prev_map = 0;

	printc("spdid: %d\n", spdid);
	/* We'll map the component into booter's heap. */
	comp_cap_info[spdid].vaddr_mapped_in_booter = (vaddr_t)cos_get_heap_ptr();
	
	for (i = 0 ; i < h->nsect ; i++) {
		struct cobj_sect *sect;
		int left;

		sect = cobj_sect_get(h, i);
		flag = MAPPING_RW;
		if (sect->flags & COBJ_SECT_KMEM) {
			flag |= MAPPING_KMEM;
		}

		dest_daddr = sect->vaddr;
		left       = cobj_sect_size(h, i);
		
		/* previous section overlaps with this one, don't remap! */
		if (round_to_page(dest_daddr) == prev_map) {
			left -= (prev_map + PAGE_SIZE - dest_daddr);
			dest_daddr = prev_map + PAGE_SIZE;
		}
		while (left > 0) {
			vaddr_t addr = cos_page_bump_alloc(&boot_info);
			assert(addr);
				
			cos_mem_alias_at(&comp_cap_info[spdid].cos_compinfo, dest_daddr, &boot_info, addr);

			prev_map = dest_daddr;
			dest_daddr += PAGE_SIZE;
			left       -= PAGE_SIZE;
		}
	}

	return 0;
}

static int
boot_comp_map_populate(struct cobj_header *h, spdid_t spdid, vaddr_t comp_info, int first_time)
{
	unsigned int i;
	/* Where are we in the actual component's memory in the booter? */
	char *start_addr, *offset;
	/* Where are we in the destination address space? */
	vaddr_t prev_daddr, init_daddr;
	struct cos_component_information *ci;

	start_addr = (char *)(comp_cap_info[spdid].vaddr_mapped_in_booter);
	init_daddr = cobj_sect_get(h, 0)->vaddr;

	for (i = 0 ; i < h->nsect ; i++) {
		struct cobj_sect *sect;
		vaddr_t dest_daddr;
		char *lsrc, *dsrc;
		int left, dest_doff;

		sect       = cobj_sect_get(h, i);
		/* virtual address in the destination address space */
		dest_daddr = sect->vaddr;
		/* where we're copying from in the cobj */
		lsrc       = cobj_sect_contents(h, i);
		/* how much is left to copy? */
		left       = cobj_sect_size(h, i);

		/* Initialize memory. */
		if (!(sect->flags & COBJ_SECT_KMEM) &&
		    (first_time || !(sect->flags & COBJ_SECT_INITONCE))) {
			if (sect->flags & COBJ_SECT_ZEROS) {
				memset(start_addr + (dest_daddr - init_daddr), 0, left);
			} else {
				memcpy(start_addr + (dest_daddr - init_daddr), lsrc, left);
			}
		}

		if (sect->flags & COBJ_SECT_CINFO) {
			assert(left == PAGE_SIZE);
			assert(comp_info == dest_daddr);
			boot_process_cinfo(h, spdid, boot_spd_end(h), start_addr + (comp_info-init_daddr), comp_info);
			ci = (struct cos_component_information*)(start_addr + (comp_info-init_daddr));
			comp_cap_info[h->id].upcall_entry = ci->cos_upcall_entry;
			printc("upcall_entry: %p\n", comp_cap_info[h->id].upcall_entry);
		}
	
	}

	return 0;
}

static int
boot_comp_map(struct cobj_header *h, spdid_t spdid, vaddr_t comp_info)
{
	prints("boot_spd_map_memory\n");
	boot_comp_map_memory(h, spdid, comp_info);
	boot_comp_map_populate(h, spdid, comp_info, 0);
	return 1;
}


extern void test_init(void);

static void
boot_create_cap_system(void)
{
	unsigned int i;


	for(i = 0; hs[i] != NULL; i++){
		
		struct cobj_header *h;
		struct cobj_sect *sect;
		captblcap_t ct;
		pgtblcap_t pt;
		compcap_t cc;
		spdid_t spdid;
		capid_t pte_cap;
		vaddr_t ci = 0;
		int n_pte = 1;
		int j, tot = 0;
		
		h = hs[i];
		
		spdid = h->id;
		
		sect = cobj_sect_get(h, 0);
		for(j = 0; j < (int)h->nsect; j++){
			tot += cobj_sect_size(h, j);
		}

		if(tot > SERVICE_SIZE){
			n_pte = tot / SERVICE_SIZE;
			if(tot % SERVICE_SIZE) n_pte++;
		}	

		ct = cos_captbl_alloc(&boot_info);
		assert(ct);	
		
		pt = cos_pgtbl_alloc(&boot_info);
		assert(pt);	

		printc("cos_upcall_entry: %p\n", cos_upcall_entry);
		/*this cos_upcall_entry will be replaced*/
		cc = cos_comp_alloc(&boot_info, ct, pt, (vaddr_t)cos_upcall_entry);
		assert(cc);	

		comp_cap_info[spdid].addr_start = sect->vaddr;

		cos_compinfo_init(&comp_cap_info[spdid].cos_compinfo, pt, ct, cc, 
				  (vaddr_t)sect->vaddr, 4, (vaddr_t)BOOT_MEM_SHM_BASE, &boot_info);

		/*
		 * The comp needs the cap to the vaddr in its pgtbl
		 *	but we don't want to allocate any memory yet.
		 *	the cos_kernel_api doens't provide this abstraction,
		 *	so I broke the abstraction layer... :/ 
		 * 
		 */
		for(j = 0; j < n_pte; j++){
			assert(__bump_mem_expand_range(&comp_cap_info[spdid].cos_compinfo, pt, sect->vaddr, 4096));
		}

		assert(h != NULL);
		assert(boot_spd_symbs(h, spdid, &ci));
		assert(boot_comp_map(h, spdid, ci));
		
		printc("Comp %d (%s) activated @ %x, size %ld!\n", h->id, h->name, sect->vaddr, tot);
		
		thdcap_t main_thd = cos_thd_alloc(&boot_info, cc, comp_cap_info[spdid].upcall_entry, 0);
        	assert(main_thd);
		int thdid = cos_introspect(&boot_info, main_thd, THD_GET_TID);
		cos_cap_cpy_at(&comp_cap_info[spdid].cos_compinfo, BOOT_CAPTBL_SELF_INITTHD_BASE, &boot_info, main_thd);	
		printc("Main thd= cap:%x tid:%x\n", (unsigned int)main_thd, thdid);
		
		cos_thd_switch(main_thd);	
	}

	return;
}
			
void 
cos_init(void)
{

	prints("Booter for new kernel\n");

 	struct cobj_header *h;
	int num_cobj, i;

	h         = (struct cobj_header *)cos_comp_info.cos_poly[0];
	num_cobj  = (int)cos_comp_info.cos_poly[1];

	deps      = (struct deps *)cos_comp_info.cos_poly[2];
	for (i = 0 ; deps[i].server ; i++) ;
	ndeps     = i;
	
	init_args = (struct component_init_str *)cos_comp_info.cos_poly[3];
	init_args++;

	boot_sched = (unsigned int *)cos_comp_info.cos_poly[4];
	
	boot_find_cobjs(h, num_cobj);

	cos_meminfo_init(&boot_info.mi, BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	
	cos_compinfo_init(&boot_info, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP, 
			(vaddr_t)cos_get_heap_ptr(), BOOT_CAPTBL_FREE, 
			(vaddr_t)BOOT_MEM_SHM_BASE, &boot_info);


	//printc("h @ %p, heap ptr @ %p\n", h, cos_get_heap_ptr());
	printc("header %p, size %d, num comps %d, new heap %p\n",
	       h, h->size, num_cobj, cos_get_heap_ptr());

	boot_create_cap_system();
	printc("booter: done creating system.\n");

	//boot_deps_run();
	
}

