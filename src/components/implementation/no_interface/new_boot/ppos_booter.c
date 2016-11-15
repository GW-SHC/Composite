#include <stdio.h>
#include <string.h>

#include <cos_component.h>
#include <cobj_format.h>
#include <cos_kernel_api.h>
#include <boot_deps.h>


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

struct cobj_header *hs[MAX_NUM_SPDS+1];

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
	for (i = 1 ; i < n ; i++) {
		int j = 0, size = 0, tot = 0;

		size = h->size;
		for (j = 0 ; j < (int)h->nsect ; j++) {
			//printc("\tsection %d, size %d\n", j, cobj_sect_size(h, j));
			tot += cobj_sect_size(h, j);
		}
		printc("cobj %s:%d found at %p:%x, size %x -> %x\n",
		       h->name, h->id, hs[i-1], size, tot, cobj_sect_get(hs[i-1], 0)->vaddr);

		end   = start + round_up_to_cacheline(size);
		hs[i] = h = (struct cobj_header*)end;
		start = end;
	}

	hs[n] = NULL;
	printc("cobj %s:%d found at %p -> %x\n",
	       hs[n-1]->name, hs[n-1]->id, hs[n-1], cobj_sect_get(hs[n-1], 0)->vaddr);
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

	//printc("h @ %p, heap ptr @ %p\n", h, cos_get_heap_ptr());
	printc("header %p, size %d, num comps %d, new heap %p\n",
	       h, h->size, num_cobj, cos_get_heap_ptr());

//	boot_create_cap_system();
/*	printc("booter: done creating system.\n");

	UNLOCK();

	boot_deps_run();
*/
	prints("lets get this party started\n");

}




















