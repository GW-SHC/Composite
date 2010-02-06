/**
 * Copyright 2007 by Gabriel Parmer, gabep1@cs.bu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Linker and loader for the Composite system: takes a collection of
 * services with their trust relationships explicitly expressed,
 * dynamically generates their stub code to connect them, communicates
 * capability information info with the runtime system, creates the
 * user-level static capability structures, and loads the services
 * into the current address space which will be used as a template for
 * the run-time system for creating each service protection domain
 * (ie. copying the entries in the pgd to new address spaces.  
 *
 * This is trusted code, and any mistakes here compromise the entire
 * system.  Essentially, control flow is restricted/created here.
 *
 * Going by the man pages, I think I might be going to hell for using
 * strtok so much.  Suffice to say, don't multithread this program.
 */

//#define HIGHEST_PRIO 1

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include <signal.h>

#include <bfd.h>

/* composite includes */
#include <spd.h>
#include <thread.h>
#include <ipc.h>

enum {PRINT_NONE = 0, PRINT_HIGH, PRINT_NORMAL, PRINT_DEBUG} print_lvl = PRINT_HIGH;

#define printl(lvl,format, args...)				\
	{							\
		if (lvl <= print_lvl) printf(format, ## args);	\
	}

#define NUM_ATOMIC_SYMBS 10 
#define NUM_KERN_SYMBS (5+NUM_ATOMIC_SYMBS)

const char *USER_CAP_TBL_NAME = "ST_user_caps";
const char *ST_INV_FN_NAME    = "ST_direct_invocation";
const char *UPCALL_ENTRY_NAME = "cos_upcall_entry";
const char *SCHED_PAGE_NAME   = "cos_sched_notifications";
const char *SPD_ID_NAME       = "cos_this_spd_id";
const char *HEAP_PTR          = "cos_heap_ptr";

const char *INIT_COMP  = "c0.o";
const char *ROOT_SCHED = "fprr.o";
const char *MPD_MGR    = "mpd.o";
const char *CONFIG_COMP = "schedconf.o";

const char *ATOMIC_USER_DEF[NUM_ATOMIC_SYMBS] = 
{ "cos_atomic_cmpxchg",
  "cos_atomic_cmpxchg_end",
  "cos_atomic_user1",
  "cos_atomic_user1_end",
  "cos_atomic_user2",
  "cos_atomic_user2_end",
  "cos_atomic_user3",
  "cos_atomic_user3_end",
  "cos_atomic_user4",
  "cos_atomic_user4_end" };

#define CAP_CLIENT_STUB_DEFAULT "SS_ipc_client_marshal_args"
#define CAP_CLIENT_STUB_POSTPEND "_call"
#define CAP_SERVER_STUB_POSTPEND "_inv"

#define BASE_SERVICE_ADDRESS SERVICE_START
#define DEFAULT_SERVICE_SIZE SERVICE_SIZE

#define LINKER_BIN "/usr/bin/ld"
#define STRIP_BIN "/usr/bin/strip"
#define GCC_BIN "gcc"

/** 
 * gabep1: the SIG_S is a section devoted to the signal handling and
 * it must be in a page on its own one page away from the rest of the
 * module 
 */
enum {TEXT_S, RODATA_S, DATA_S, BSS_S, EH_FRAME_S, MAXSEC_S};

struct sec_info {
	asection *s;
	int offset;
};

unsigned long ro_start;
unsigned long data_start;

char script[64];
char tmp_exec[128];

struct sec_info srcobj[MAXSEC_S];
struct sec_info ldobj[MAXSEC_S];

#define UNDEF_SYMB_TYPE 0x1
#define EXPORTED_SYMB_TYPE 0x2
#define MAX_SYMBOLS 256
#define MAX_TRUSTED 32
#define MAX_SYMB_LEN 256

typedef int (*observer_t)(asymbol *, void *data);

struct service_symbs;

struct symb {
	char *name;
	vaddr_t addr;
	struct service_symbs *exporter;
	struct symb *exported_symb;
};

struct symb_type {
	int num_symbs;
	struct symb symbs[MAX_SYMBOLS];

	struct service_symbs *parent;
};

struct dependency {
	struct service_symbs *dep;
	char *modifier;
	int mod_len;
};

struct service_symbs {
	char *obj, *init_str;
	unsigned long lower_addr, size, heap_top;

	int is_scheduler;
	struct service_symbs *scheduler;
	
	struct spd *spd;
	struct symb_type exported, undef;
	int num_dependencies;
	struct dependency dependencies[MAX_TRUSTED];
	struct service_symbs *next;
	int depth;

	void *extern_info;
};

static unsigned long getsym(bfd *obj, char* symbol)
{
	long storage_needed;
	asymbol **symbol_table;
	long number_of_symbols;
	int i;
	
	storage_needed = bfd_get_symtab_upper_bound (obj);
	
	if (storage_needed <= 0){
		printl(PRINT_DEBUG, "no symbols in object file\n");
		exit(-1);
	}
	
	symbol_table = (asymbol **) malloc (storage_needed);
	number_of_symbols = bfd_canonicalize_symtab(obj, symbol_table);

	//notes: symbol_table[i]->flags & (BSF_FUNCTION | BSF_GLOBAL)
	for (i = 0; i < number_of_symbols; i++) {
		if(!strcmp(symbol, symbol_table[i]->name)){
			return symbol_table[i]->section->vma + symbol_table[i]->value;
		} 
	}

	printl(PRINT_DEBUG, "Unable to find symbol named %s\n", symbol);
	return 0;
}

#ifdef DEBUG
static void print_syms(bfd *obj)
{
	long storage_needed;
	asymbol **symbol_table;
	long number_of_symbols;
	int i;
	
	storage_needed = bfd_get_symtab_upper_bound (obj);
	
	if (storage_needed <= 0){
		printl(PRINT_DEBUG, "no symbols in object file\n");
		exit(-1);
	}
	
	symbol_table = (asymbol **) malloc (storage_needed);
	number_of_symbols = bfd_canonicalize_symtab(obj, symbol_table);

	//notes: symbol_table[i]->flags & (BSF_FUNCTION | BSF_GLOBAL)
	for (i = 0; i < number_of_symbols; i++) {
		printl(PRINT_DEBUG, "name: %s, addr: %d, flags: %s, %s%s%s, in sect %s%s%s.\n",  
		       symbol_table[i]->name,
		       (unsigned int)(symbol_table[i]->section->vma + symbol_table[i]->value),
		       (symbol_table[i]->flags & BSF_GLOBAL) ? "global" : "local", 
		       (symbol_table[i]->flags & BSF_FUNCTION) ? "function" : "data",
		       symbol_table[i]->flags & BSF_SECTION_SYM ? ", section": "", 
		       symbol_table[i]->flags & BSF_FILE ? ", file": "", 
		       symbol_table[i]->section->name, 
		       bfd_is_und_section(symbol_table[i]->section) ? ", undefined" : "", 
		       symbol_table[i]->section->flags & SEC_RELOC ? ", relocate": "");
		//if(!strcmp(executive_entry_symbol, symbol_table[i]->name)){
		//return symbol_table[i]->section->vma + symbol_table[i]->value;
		//} 
	}

	free(symbol_table);
	//printl(PRINT_DEBUG, "Unable to find symbol named %s\n", executive_entry_symbol);
	//return -1;
	return;
}
#endif

static void findsections(bfd *abfd, asection *sect, PTR obj)
{
	struct sec_info *sec_arr = obj;
	
	if(!strcmp(sect->name, ".text")){
		sec_arr[TEXT_S].s = sect;
	}
	else if(!strcmp(sect->name, ".data")){
		sec_arr[DATA_S].s = sect;
	}
	else if(!strcmp(sect->name, ".rodata")){
		sec_arr[RODATA_S].s = sect;
	}
	else if(!strcmp(sect->name, ".bss")){
		sec_arr[BSS_S].s = sect;
	}
	else if (!strcmp(sect->name, ".eh_frame")) {
		sec_arr[EH_FRAME_S].s = sect;
	}
}

static int calc_offset(int offset, asection *sect)
{
	int align;
	
	if(!sect){
		return offset;
	}
	align = (1 << sect->alignment_power) - 1;
	
	if(offset & align){
		offset -= offset & align;
		offset += align + 1;
	}
	return offset;
}

static int calculate_mem_size(int first, int last) 
{
	int offset = 0;
	int i;
	
	for (i = first; i < last; i++){
		if(srcobj[i].s == NULL) {
			printl(PRINT_DEBUG, "Warning: could not find section for sectno %d.\n", i);
			continue;
		}
		offset = calc_offset(offset, srcobj[i].s);
		srcobj[i].offset = offset;
		offset += bfd_get_section_size(srcobj[i].s);
	}
	
	return offset;
}

static void emit_address(FILE *fp, unsigned long addr)
{
	fprintf(fp, ". = 0x%x;\n", (unsigned int)addr);
}

static void emit_section(FILE *fp, char *sec)
{
	/*
	 * The kleene star after the section will collapse
	 * .rodata.str1.x for all x into the only case we deal with
	 * which is .rodata
	 */
//	fprintf(fp, ".%s : { *(.%s*) }\n", sec, sec);
	fprintf(fp, ".%s : { *(.%s*) }\n", sec, sec);
}

/* Look at sections and determine sizes of the text and
 * and data portions of the object file once loaded */

static int genscript(int with_addr)
{
	FILE *fp;
	static unsigned int cnt = 0;
	
	sprintf(script, "/tmp/loader_script.%d", getpid());
	sprintf(tmp_exec, "/tmp/loader_exec.%d.%d.%d", with_addr, getpid(), cnt);
	cnt++;

	fp = fopen(script, "w");
	if(fp == NULL){
		perror("fopen failed");
		exit(-1);
	}
	
	fprintf(fp, "SECTIONS\n{\n");

	//if (with_addr) emit_address(fp, sig_start);
	//emit_section(fp, "signal_handling");
	if (with_addr) emit_address(fp, ro_start);
	emit_section(fp, "text");
	emit_section(fp, "rodata");
	if (with_addr) emit_address(fp, data_start);
	emit_section(fp, "data");
	emit_section(fp, "bss");
	if (with_addr) emit_address(fp, 0);
	emit_section(fp, "eh_frame");
	
	fprintf(fp, "}\n");
	fclose(fp);

	return 0;
}

static void run_linker(char *input_obj, char *output_exe)
{
	char linker_cmd[256];
	sprintf(linker_cmd, LINKER_BIN " -T %s -o %s %s", script, output_exe,
		input_obj);
	printl(PRINT_DEBUG, linker_cmd);
	fflush(stdout);
	system(linker_cmd);
}

#define bfd_sect_size(bfd, x) (bfd_get_section_size(x)/bfd_octets_per_byte(bfd))

int set_object_addresses(bfd *obj, struct service_symbs *obj_data)
{
	struct symb_type *st = &obj_data->exported;
	int i;

/* debug:
	unsigned int *retaddr;
	char **blah;
	typedef int (*fn_t)(void);
	fn_t fn;
	char *str = "hi, this is";

	if ((retaddr = (unsigned int*)getsym(obj, "ret")))
		printl(PRINT_DEBUG, "ret %x is %d.\n", (unsigned int)retaddr, *retaddr);
	if ((retaddr = (unsigned int*)getsym(obj, "blah")))
		printl(PRINT_DEBUG, "blah %x is %d.\n", (unsigned int)retaddr, *retaddr);
	if ((retaddr = (unsigned int*)getsym(obj, "zero")))
		printl(PRINT_DEBUG, "zero %x is %d.\n", (unsigned int)retaddr, *retaddr);
	if ((blah = (char**)getsym(obj, "str")))
		printl(PRINT_DEBUG, "str %x is %s.\n", (unsigned int)blah, *blah);
	if ((retaddr = (unsigned int*)getsym(obj, "other")))
		printl(PRINT_DEBUG, "other %x is %d.\n", (unsigned int)retaddr, *retaddr);
	if ((fn = (fn_t)getsym(obj, "foo")))
		printl(PRINT_DEBUG, "retval from foo: %d (%d).\n", fn(), (int)*str);
*/
	for (i = 0 ; i < st->num_symbs ; i++) {
		char *symb = st->symbs[i].name;
		unsigned long addr = getsym(obj, symb);
/*
		printl(PRINT_DEBUG, "Symbol %s at address 0x%x.\n", symb, 
		       (unsigned int)addr);
*/
		if (addr == 0) {
			printl(PRINT_DEBUG, "Symbol %s has invalid address.\n", symb);
			return -1;
		}

		st->symbs[i].addr = addr;
	}
	
	return 0;
}

vaddr_t get_symb_address(struct symb_type *st, const char *symb)
{
	int i;

	for (i = 0 ; i < st->num_symbs ; i++ ) {
		if (!strcmp(st->symbs[i].name, symb)) {
			return st->symbs[i].addr;
		}
	}
	return 0;
}

static int load_service(struct service_symbs *ret_data, unsigned long lower_addr, 
			unsigned long size)
{
	bfd *obj, *objout;
	void *tmp_storage;

	int text_size, ro_size;
	int alldata_size;
	void *ret_addr;
	char *service_name = ret_data->obj; 

	int i;

	if (!service_name) {
		printl(PRINT_DEBUG, "Invalid path to executive.\n");
		return -1;
	}

	printl(PRINT_NORMAL, "Processing object %s:\n", service_name);

	genscript(0);
	run_linker(service_name, tmp_exec);
	
	obj = bfd_openr(tmp_exec, "elf32-i386");
	if(!obj){
		bfd_perror("object open failure");
		return -1;
	}
	if(!bfd_check_format(obj, bfd_object)){
		printl(PRINT_DEBUG, "Not an object file!\n");
		return -1;
	}
	
	for (i = 0 ; i < MAXSEC_S ; i++) srcobj[i].s = NULL;
	bfd_map_over_sections(obj, findsections, srcobj);

	ro_start = lower_addr;
	/* Determine the size of and allocate the text and Read-Only data area */
	text_size = calculate_mem_size(TEXT_S, RODATA_S);
	ro_size = calculate_mem_size(RODATA_S, DATA_S);
	printl(PRINT_DEBUG, "\tRead only text (%x) and data section (%x): %x:%x.\n",
	       (unsigned int)text_size, (unsigned int)ro_size, 
	       (unsigned int)ro_start, (unsigned int)text_size+ro_size);

	/* see calculate_mem_size for why we do this...not intelligent */
	ro_size = calculate_mem_size(TEXT_S, DATA_S);
	ro_size = round_up_to_page(ro_size);

	/**
	 * FIXME: needing PROT_WRITE is daft, should write to file,
	 * then map in ro
	 */
	assert(0 != ro_size);
	ret_addr = mmap((void*)ro_start, ro_size,
			PROT_EXEC | PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
			0, 0);
	if (MAP_FAILED == ret_addr){
		/* If you get outrageous sizes here, there is a
		 * required section missing (such as rodata).  Add a
		 * const. */
		printl(PRINT_DEBUG, "Error mapping 0x%x(0x%x)\n", 
		       (unsigned int)ro_start, (unsigned int)ro_size);
		perror("Couldn't map text segment into address space");
		return -1;
	}
	
	data_start = ro_start + ro_size;
	/* Allocate the read-writable areas .data .bss */
	alldata_size = calculate_mem_size(DATA_S, MAXSEC_S);

	printl(PRINT_DEBUG, "\tData section: %x:%x\n",
	       (unsigned int)data_start, (unsigned int)alldata_size);

	alldata_size = round_up_to_page(alldata_size);
	if (alldata_size != 0) {
		ret_addr = mmap((void*)data_start, alldata_size,
				PROT_WRITE | PROT_READ,
				MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
				0, 0);

		if (MAP_FAILED == ret_addr){
			perror("Couldn't map data segment into address space");
			return -1;
		}
	}
	
	tmp_storage = malloc(ro_size > alldata_size ? ro_size : alldata_size);
	if(tmp_storage == NULL)
	{
		perror("Memory allocation failed\n");
		return -1;
	}
	
	unlink(tmp_exec);
	genscript(1);
	run_linker(service_name, tmp_exec);
	unlink(script);
	objout = bfd_openr(tmp_exec, "elf32-i386");
	if(!objout){
		bfd_perror("Object open failure\n");
		return -1;
	}
	if(!bfd_check_format(objout, bfd_object)){
		printl(PRINT_DEBUG, "Not an object file!\n");
		return -1;
	}
	
	bfd_map_over_sections(objout, findsections, &ldobj[0]);

	/* get the text and ro sections in a buffer */
	bfd_get_section_contents(objout, ldobj[TEXT_S].s,
				 tmp_storage + srcobj[TEXT_S].offset, 0,
				 bfd_sect_size(objout, srcobj[TEXT_S].s));
	printl(PRINT_DEBUG, "\tretreiving TEXT at offset %d of size %x.\n", 
	       srcobj[TEXT_S].offset, (unsigned int)bfd_sect_size(objout, srcobj[TEXT_S].s));

	if(srcobj[RODATA_S].s && ldobj[RODATA_S].s){
		bfd_get_section_contents(objout, ldobj[RODATA_S].s,
					 tmp_storage + srcobj[RODATA_S].offset, 0,
					 bfd_sect_size(objout, srcobj[RODATA_S].s));
		printl(PRINT_DEBUG, "\tretreiving RODATA at offset %d of size %x.\n", 
		       srcobj[RODATA_S].offset, (unsigned int)bfd_sect_size(objout, srcobj[RODATA_S].s));
	}

	
	/* 
	 * ... and copy that buffer into the actual memory location
	 * object is linked for (load it!)
	 */
	printl(PRINT_DEBUG, "\tCopying RO to %x from %x of size %x.\n", 
	       (unsigned int) ro_start, (unsigned int)tmp_storage, (unsigned int)ro_size);
	memcpy((void*)ro_start, tmp_storage, ro_size);

	printl(PRINT_DEBUG, "\tretreiving DATA at offset %x of size %x.\n", 
	       srcobj[DATA_S].offset, (unsigned int)bfd_sect_size(objout, srcobj[DATA_S].s));
	printl(PRINT_DEBUG, "\tCopying data from object to %x:%x.\n", 
	       (unsigned int)tmp_storage + srcobj[DATA_S].offset, 
	       (unsigned int)bfd_sect_size(obj, srcobj[DATA_S].s));

	/* and now do the same for the data and BSS */
	bfd_get_section_contents(objout, ldobj[DATA_S].s,
				 tmp_storage + srcobj[DATA_S].offset, 0,
				 bfd_sect_size(obj, srcobj[DATA_S].s));

	printl(PRINT_DEBUG, "\tretreiving BSS at offset %x of size %x.\n", 
	       srcobj[BSS_S].offset, (unsigned int)bfd_sect_size(objout, srcobj[BSS_S].s));
	printl(PRINT_DEBUG, "\tZeroing out BSS from %x of size %x.\n", 
	       (unsigned int)tmp_storage + srcobj[BSS_S].offset,
	       (unsigned int)bfd_sect_size(obj, srcobj[BSS_S].s));

	/* Zero bss */
	memset(tmp_storage + srcobj[BSS_S].offset, 0,
	       bfd_sect_size(obj, srcobj[BSS_S].s));

	printl(PRINT_DEBUG, "\tCopying DATA to %x from %x of size %x.\n", 
	       (unsigned int) data_start, (unsigned int)tmp_storage, (unsigned int)alldata_size);
	
	memcpy((void*)data_start, tmp_storage, alldata_size);
	
	free(tmp_storage);

	if (set_object_addresses(objout, ret_data)) {
		printl(PRINT_DEBUG, "Could not find all object symbols.\n");
		return -1;
	}

	ret_data->lower_addr = lower_addr;
	ret_data->size = size;
	ret_data->heap_top = (int)data_start + alldata_size;
	
	bfd_close(obj);
	bfd_close(objout);

	printl(PRINT_NORMAL, "Object %s processed as %s with script %s.\n", 
	       service_name, tmp_exec, script);
//	unlink(tmp_exec);

	return 0;

	/* 
	 * FIXME: unmap sections, free memory, unlink file, close bfd
	 * objects, etc... for the various error positions, i.e. all
	 * return -1;s
	 */
}

/* FIXME: should modify code to use
static struct service_symbs *get_dependency_by_index(struct service_symbs *s,
						     int index)
{
	if (index >= s->num_dependencies) {
		return NULL;
	}

	return s->dependencies[index].dep;
}
*/
static int add_service_dependency(struct service_symbs *s, 
				  struct service_symbs *dep)
{
	struct dependency *d;

	if (!s || !dep || s->num_dependencies == MAX_TRUSTED) {
		return -1;
	}

	d = &s->dependencies[s->num_dependencies];
	d->dep = dep;
	d->modifier = NULL;
	s->num_dependencies++;

	return 0;
}

static int add_modified_service_dependency(struct service_symbs *s,
					   struct service_symbs *dep, 
					   char *modifier, int mod_len)
{
	struct dependency *d;
	char *new_mod;

	assert(modifier);
	new_mod = malloc(mod_len+1);
	assert(new_mod);
	memcpy(new_mod, modifier, mod_len);
	new_mod[mod_len] = '\0';

	if (!s || !dep || s->num_dependencies == MAX_TRUSTED) {
		return -1;
	}

	d = &s->dependencies[s->num_dependencies];
	d->dep = dep;
	d->modifier = new_mod;
	d->mod_len = mod_len;
	s->num_dependencies++;

	return 0;
}

static int initialize_service_symbs(struct service_symbs *str)
{
	memset(str, 0, sizeof(struct service_symbs));
	str->exported.parent = str;
	str->undef.parent = str;
	str->next = NULL;
	str->exported.num_symbs = str->undef.num_symbs = 0;
	str->num_dependencies = 0;
	str->depth = -1;

	return 0;
}

static struct service_symbs *alloc_service_symbs(char *obj)
{
	struct service_symbs *str;
	char *obj_name = malloc(strlen(obj)+1), *cpy, *orig, *pos;
	const char lassign = '(', *rassign = ")", *assign = "=";
	int sched = 0;

	assert(obj_name);
	/* Do we have a value assignment (a component copy)?  Syntax
	 * is (newval=oldval),... */
	if (obj[0] == lassign) {
		char copy_cmd[256];
		int ret, off = 1;

		if (obj[1] == '*') {
			sched = 1;
			off = 2;
		}
		cpy = strtok_r(obj+off, assign, &pos);
		orig = strtok_r(pos, rassign, &pos);
		sprintf(copy_cmd, "cp %s %s", orig, cpy);
		ret = system(copy_cmd);
		assert(-1 != ret);
		obj = cpy;
	} else if (obj[0] == '*') {
		sched = 1;
		obj = obj+1;
	}

	str = malloc(sizeof(struct service_symbs));
	if (!str || initialize_service_symbs(str)) {
		return NULL;
	}

	strcpy(obj_name, obj);
	str->obj = obj_name;

	str->is_scheduler = sched;
	str->scheduler = NULL;

	return str;
}

static void free_symbs(struct symb_type *st)
{
	int i;

	for (i = 0 ; i < st->num_symbs ; i++) {
		free(st->symbs[i].name);
	}
}

static void free_service_symbs(struct service_symbs *str)
{
	free(str->obj);
	free_symbs(&str->exported);
	free_symbs(&str->undef);
	free(str);

	return;
}

static int obs_serialize(asymbol *symb, void *data)
{
	struct symb_type *symbs = data;
	char *name;

	/* So that we can later add into the exported symbols the user
         * capability table
	 */
	if (symbs->num_symbs >= MAX_SYMBOLS-NUM_KERN_SYMBS) {
		printl(PRINT_DEBUG, "Have exceeded the number of allowed "
		       "symbols for object %s.\n", symbs->parent->obj);
		return -1;
	}

	/* Ignore main */
	if (!strcmp("main", symb->name)) {
		return 0;
	}

	name = malloc(strlen(symb->name) + 1);
	strcpy(name, symb->name);
	
	symbs->symbs[symbs->num_symbs].name = name;
	symbs->symbs[symbs->num_symbs].addr = 0;
	symbs->num_symbs++;

	return 0;
}

static int for_each_symb_type(bfd *obj, int symb_type, observer_t o, void *obs_data)
{
	long storage_needed;
	asymbol **symbol_table;
	long number_of_symbols;
	int i;
	
	storage_needed = bfd_get_symtab_upper_bound (obj);
	
	if (storage_needed <= 0){
		printl(PRINT_DEBUG, "no symbols in object file\n");
		exit(-1);
	}
	
	symbol_table = (asymbol **) malloc (storage_needed);
	number_of_symbols = bfd_canonicalize_symtab(obj, symbol_table);

	for (i = 0; i < number_of_symbols; i++) {
		/* 
		 * Invoke the observer if we are interested in a type,
		 * and the symbol is of that type where type is either
		 * undefined or exported, currently
		 */
		if ((symb_type & UNDEF_SYMB_TYPE &&
		    bfd_is_und_section(symbol_table[i]->section)) 
		    ||
		    (symb_type & EXPORTED_SYMB_TYPE &&
		    symbol_table[i]->flags & BSF_FUNCTION &&
		    symbol_table[i]->flags & BSF_GLOBAL)) {
			if ((*o)(symbol_table[i], obs_data)) {
				return -1;
			}
		}
	}

	free(symbol_table);

	return 0;
}

/*
 * Fill in the symbols of service_symbs for the object passed in as
 * the tmp_exec
 */
static int obj_serialize_symbols(char *tmp_exec, int symb_type, struct service_symbs *str) 
{
 	bfd *obj; 
	struct symb_type *st;

	obj = bfd_openr(tmp_exec, "elf32-i386");
	if(!obj){
		printl(PRINT_HIGH, "Attempting to open %s\n", str->obj);
		bfd_perror("Object open failure");
		return -1;
	}
	
	if(!bfd_check_format(obj, bfd_object)){
		printl(PRINT_DEBUG, "Not an object file!\n");
		return -1;
	}
	
	if (symb_type == UNDEF_SYMB_TYPE) {
		st = &str->undef;
	} else if (symb_type == EXPORTED_SYMB_TYPE) {
		st = &str->exported;
	}
	for_each_symb_type(obj, symb_type, obs_serialize, st);

	bfd_close(obj);

	return 0;
}

static inline void print_symbs(struct symb_type *st)
{
	int i;

	for (i = 0 ; i < st->num_symbs ; i++) {
		printl(PRINT_DEBUG, "%s, ", st->symbs[i].name);
	}

	return;
}

static void print_objs_symbs(struct service_symbs *str)
{
	if (print_lvl < PRINT_DEBUG) return;
	while (str) {
		printl(PRINT_DEBUG, "Service %s:\n\tExports: ", str->obj);
		print_symbs(&str->exported);
		printl(PRINT_DEBUG, "\n\tUndefined: ");
		print_symbs(&str->undef);
		printl(PRINT_DEBUG, "\n\n");

		str = str->next;
	}

	return;
}

/*
 * Has this service already been processed?
 */
static inline int service_processed(char *obj_name, struct service_symbs *services)
{
	while (services) {
		if (!strcmp(services->obj, obj_name)) {
			return 1;
		}
		services = services->next;
	}

	return 0;
}

static inline void add_kexport(struct service_symbs *ss, const char *name)
{
	struct symb_type *ex = &ss->exported;
	
	ex->symbs[ex->num_symbs].name = malloc(strlen(name)+1);
	strcpy(ex->symbs[ex->num_symbs].name, name);
	ex->num_symbs++;
	
	return;
}

/* 
 * Assume that these are added LAST.  The last NUM_KERN_SYMBS are
 * ignored for most purposes so they must be the actual kern_syms.
 *
 * The kernel needs to know where a few symbols are, add them:
 * user_caps, cos_sched_notifications
 */
static void add_kernel_exports(struct service_symbs *service)
{
	int i;

	add_kexport(service, USER_CAP_TBL_NAME);
	add_kexport(service, SCHED_PAGE_NAME);
	add_kexport(service, SPD_ID_NAME);
	add_kexport(service, HEAP_PTR);
	for (i = 0 ; i < NUM_ATOMIC_SYMBS ; i++) {
		add_kexport(service, ATOMIC_USER_DEF[i]);
	}

	return;
}

/* 
 * Obtain the list of undefined and exported symbols for a collection
 * of services.
 *
 * services is an array of comma delimited addresses to the services
 * we wish to get the symbol information for.  Note that all ',' in
 * the services string are replaced with '\0', and that this function
 * is not thread-safe due to use of strtok.
 *
 * Returns a linked list struct service_symbs data structure with the
 * arrays within each service_symbs filled in to reflect the symbols
 * within that service.
 */
static struct service_symbs *prepare_service_symbs(char *services)
{
	struct service_symbs *str, *first;
	const char *init_delim = ",", *serv_delim = ";";
	char *tok, *init_str;
	int len;
	
	tok = strtok(services, init_delim);
	first = str = alloc_service_symbs(tok);
	init_str = strtok(NULL, serv_delim);
	len = strlen(init_str)+1;
	str->init_str = malloc(len);
	assert(str->init_str);
	memcpy(str->init_str, init_str, len);

	do {
		if (obj_serialize_symbols(str->obj, EXPORTED_SYMB_TYPE, str) ||
		    obj_serialize_symbols(str->obj, UNDEF_SYMB_TYPE, str)) {
			printl(PRINT_DEBUG, "Could not operate on object %s: error.\n", tok);
			return NULL;
		}
		add_kernel_exports(str);
		tok = strtok(NULL, init_delim);
		if (tok) {
			str->next = alloc_service_symbs(tok);
			str = str->next;

			init_str = strtok(NULL, serv_delim);
			len = strlen(init_str)+1;
			str->init_str = malloc(len);
			assert(str->init_str);
			memcpy(str->init_str, init_str, len);
		}
	} while (tok);
		
	return first;
}


/*
 * Find the exporter for a specific symbol from amongst a list of
 * exporters.
 */
static inline 
struct service_symbs *find_symbol_exporter(struct symb *s, 
					   struct dependency *exporters,
					   int num_exporters, struct symb **exported)
{
	int i,j;

	for (i = 0 ; i < num_exporters ; i++) {
		struct dependency *exporter;
		struct symb_type *exp_symbs;

		exporter = &exporters[i];
		exp_symbs = &exporter->dep->exported;

		for (j = 0 ; j < exp_symbs->num_symbs ; j++) {
			if (!strcmp(s->name, exp_symbs->symbs[j].name)) {
				*exported = &exp_symbs->symbs[j];
				return exporters[i].dep;
			}
			if (exporter->modifier && 
			    !strncmp(s->name, exporter->modifier, exporter->mod_len) && 
			    !strcmp(s->name + exporter->mod_len, exp_symbs->symbs[j].name)) {
				*exported = &exp_symbs->symbs[j];
				return exporters[i].dep;
			}
		}
	}

	return NULL;
}

/*
 * Verify that all symbols can be resolved by the present dependency
 * relations.  This is an equivalent to programming language
 * "completeness".
 *
 * Assumptions: All exported and undefined symbols are defined for
 * each service (prepare_service_symbs has been called), and that the
 * tree of services has been established designating the dependents of
 * each service (process_dependencies has been called).
 */
static int verify_dependency_completeness(struct service_symbs *services)
{
	int ret = 0;
	int i;

	/* for each of the services... */
	while (services) {
		struct symb_type *undef_symbs = &services->undef;

		/* ...go through each of its undefined symbols... */
		for (i = 0 ; i < undef_symbs->num_symbs ; i++) {
			struct symb *symb = &undef_symbs->symbs[i];
			struct symb *exp_symb;
			struct service_symbs *exporter;

			/* 
			 * ...and make sure they are matched to an
			 * exported function in a service we are
			 * dependent on.
			 */
			exporter = find_symbol_exporter(symb, services->dependencies, 
							services->num_dependencies, &exp_symb);

			if (!exporter) {
				printl(PRINT_HIGH, "Could not find exporter of symbol %s in service %s.\n",
				       symb->name, services->obj);

				ret = -1;
				goto exit;
			} else {
				symb->exporter = exporter;
				symb->exported_symb = exp_symb;
			}

			if (exporter->is_scheduler) {
				if (NULL == services->scheduler) {
					services->scheduler = exporter;
					//printl(PRINT_HIGH, "%s has scheduler %s.\n", services->obj, exporter->obj);
				} else if (exporter != services->scheduler) {
					printl(PRINT_HIGH, "Service %s is dependent on more than one scheduler (at least %s and %s).  Error.\n", services->obj, exporter->obj, services->scheduler->obj);
					ret = -1;
					goto exit;
				}
			}
		}
		
		services = services->next;
	}

 exit:
	return ret;
}

static int rec_verify_dag(struct service_symbs *services,
			  int current_depth, int max_depth)
{
	int i;

	/* cycle */
	if (current_depth > max_depth) {
		return -1;
	}

	if (current_depth > services->depth) {
		services->depth = current_depth;
	}

	for (i = 0 ; i < services->num_dependencies ; i++) {
		struct service_symbs *d = services->dependencies[i].dep;

		if (rec_verify_dag(d, current_depth+1, max_depth)) {
			return -1;
		}
	}

	return 0;
}

/*
 * FIXME: does not check for disjoint graphs at this time.
 *
 * The only soundness we can really check here is that services are
 * arranged in a DAG, i.e. that no cycles exist.  O(N^2*E).
 *
 * Assumptions: All exported and undefined symbols are defined for
 * each service (prepare_service_symbs has been called), and that the
 * tree of services has been established designating the dependents of
 * each service (process_dependencies has been called).
 */
static int verify_dependency_soundness(struct service_symbs *services)
{
	struct service_symbs *tmp_s = services;
	int cnt = 0;

	while (tmp_s) {
		cnt++;
		tmp_s = tmp_s->next;
	}

	while (services) {
		if (rec_verify_dag(services, 0, cnt)) {
			printl(PRINT_DEBUG, "Cycle found in dependencies.  Not linking.\n");
			return -1;
		}

		services = services->next;
	}

	return 0;
}

static inline struct service_symbs *get_service_struct(char *name, 
						       struct service_symbs *list)
{
	while (list) {
		assert(name);
		assert(list && list->obj);
		if (!strcmp(name, list->obj)) {
			return list;
		}

		list = list->next;
	}

	return NULL;
}

/*
 * Add to the service_symbs structures the dependents.
 * 
 * deps is formatted as "sa-sb|sc|...|sn;sd-se|sf|...;...", or a list
 * of "service" hyphen "dependencies...".  In the above example, sa
 * depends on functions within sb, sc, and sn.
 */
static int deserialize_dependencies(char *deps, struct service_symbs *services)
{
	char *next, *current;
	char *serial = "-";
	char *parallel = "|";
	char inter_dep = ';';
	char open_modifier = '[';
	char close_modifier = ']';

	if (!deps) return -1;
	next = current = deps;

	/* go through each dependent-trusted|... relation */
	while (current) {
		struct service_symbs *s, *dep;
		char *tmp;

		next = strchr(current, inter_dep);
		if (next) {
			*next = '\0';
			next++;
		}
		/* the dependent */
		tmp = strtok(current, serial);
		s = get_service_struct(tmp, services);
		if (!s) {
			printl(PRINT_HIGH, "Could not find service %s.\n", tmp);
			return -1;
		}

		/* go through the | invoked services */
		tmp = strtok(NULL, parallel);
		while (tmp) {
			char *mod = NULL;
			/* modifier! */
			if (tmp[0] == open_modifier) {
				mod = tmp+1;
				tmp = strchr(tmp, close_modifier);
				if (!tmp) {
					printl(PRINT_HIGH, "Could not find closing modifier ] in %s\n", mod);
					return -1;
				}
				*tmp = '\0';
				tmp++;
				assert(mod);
				assert(tmp);
			}
			dep = get_service_struct(tmp, services);
			if (!dep) {
				printl(PRINT_HIGH, "Could not find service %s.\n", tmp);
				return -1;
			} 
			if (dep == s) {
				printl(PRINT_HIGH, "Reflexive relations not allowed (for %s).\n", 
				       s->obj);
				return -1;
			}

			if (mod) add_modified_service_dependency(s, dep, mod, strlen(mod));
			else add_service_dependency(s, dep);
			tmp = strtok(NULL, parallel);
		} 

		current = next;
	}

	return 0;
}

static char *strip_prepended_path(char *name)
{
	char *tmp;

	tmp = strrchr(name, '/');

	if (!tmp) {
		return name;
	} else {
		return tmp+1;
	}
}

/*
 * Produces a number of object files in /tmp named objname.o.pid.o
 * with no external dependencies.
 *
 * gen_stub_prog is the address to the client stub generation prog
 * st_object is the address of the symmetric trust object.
 *
 * This is kind of a big hack.
 */
static void gen_stubs_and_link(char *gen_stub_prog, struct service_symbs *services)
{
	int pid = getpid();
	char tmp_str[2048];

	while (services) {
		int i;
		struct symb_type *symbs = &services->undef;
		char dest[256];
		char tmp_name[256];
		char *obj_name, *orig_name, *str;

		orig_name = services->obj;
		obj_name = strip_prepended_path(services->obj);
		sprintf(tmp_name, "/tmp/%s.%d", obj_name, pid);
		
/*		if (symbs->num_symbs == 0) {
			sprintf(tmp_str, "cp %s %s.o", 
				orig_name, tmp_name);
			system(tmp_str);

			str = malloc(strlen(tmp_name)+3);
			strcpy(str, tmp_name);
			strcat(str, ".o");
			free(services->obj);
			services->obj = str;

			services = services->next;
			continue;
		}
*/
		/* make the command line for an invoke the stub generator */
		strcpy(tmp_str, gen_stub_prog);

		if (symbs->num_symbs > 0) {
			strcat(tmp_str, " ");
			strcat(tmp_str, symbs->symbs[0].name);
		}
		for (i = 1 ; i < symbs->num_symbs ; i++) {
			strcat(tmp_str, ",");
			strcat(tmp_str, symbs->symbs[i].name);
		}

		/* invoke the stub generator */
		sprintf(dest, " > %s_stub.S", tmp_name);
		strcat(tmp_str, dest);
		system(tmp_str);

		/* compile the stub */
		sprintf(tmp_str, GCC_BIN " -c -o %s_stub.o %s_stub.S", 
			tmp_name, tmp_name);
		system(tmp_str);

		/* link the stub to the service */
		sprintf(tmp_str, LINKER_BIN " -r -o %s.o %s %s_stub.o", 
			tmp_name, orig_name, tmp_name);
		system(tmp_str);

		/* Make service names reflect their new linked versions */
		str = malloc(strlen(tmp_name)+3);
		strcpy(str, tmp_name);
		strcat(str, ".o");
		free(services->obj);
		services->obj = str;
		
		sprintf(tmp_str, "rm %s_stub.o %s_stub.S", tmp_name, tmp_name);
		system(tmp_str);

		services = services->next;
	}

	return;
}

/*
 * Load into the current address space all of the services.  
 *
 * FIXME: Load intelligently, from the most trusted to the least in
 * some order instead of randomly.  This will be important when we do
 * dynamically loading.
 *
 * Assumes that a file exists for each service in /tmp/service.o.pid.o
 * (i.e. that gen_stubs_and_link has been called.)
 */
static int load_all_services(struct service_symbs *services)
{
	unsigned long service_addr = BASE_SERVICE_ADDRESS;

	service_addr += DEFAULT_SERVICE_SIZE;

	while (services) {
		if (load_service(services, service_addr, DEFAULT_SERVICE_SIZE)) {
			return -1;
		}

		service_addr += DEFAULT_SERVICE_SIZE;

		printl(PRINT_DEBUG, "\n");
		services = services->next;
	}

	return 0;
}

static void print_kern_symbs(struct service_symbs *services)
{
	const char *u_tbl = USER_CAP_TBL_NAME;

	while (services) {
		vaddr_t addr;

		if ((addr = get_symb_address(&services->exported, u_tbl))) {
			printl(PRINT_DEBUG, "Service %s:\n\tusr_cap_tbl: %x\n",
			       services->obj, (unsigned int)addr);
		}
		
		services = services->next;
	}
}

/* static void add_spds(struct service_symbs *services) */
/* { */
/* 	struct service_symbs *s = services; */
	
/* 	/\* first, make sure that all services have spds *\/ */
/* 	while (s) { */
/* 		int num_undef = s->undef.num_symbs; */
/* 		struct usr_inv_cap *ucap_tbl; */

/* 		ucap_tbl = (struct usr_inv_cap*)get_symb_address(&s->exported,  */
/* 								 USER_CAP_TBL_NAME); */
/* 		/\* no external dependencies, no caps *\/ */
/* 		if (!ucap_tbl) { */
/* 			s->spd = spd_alloc(0, MNULL); */
/* 		} else { */
/* //			printl(PRINT_DEBUG, "Requesting %d caps.\n", num_undef); */
/* 			s->spd = spd_alloc(num_undef, ucap_tbl); */
/* 		} */

/* //		printl(PRINT_DEBUG, "Service %s has spd %x.\n", s->obj, (unsigned int)s->spd); */

/* 		s = s->next; */
/* 	} */

/* 	/\* then add the capabilities *\/ */
/* 	while (services) { */
/* 		int i; */
/* 		int num_undef = services->undef.num_symbs; */

/* 		for (i = 0 ; i < num_undef ; i++) { */
/* 			struct spd *owner_spd, *dest_spd; */
/* 			struct service_symbs *dest_service; */
/* 			vaddr_t dest_entry_fn; */
/* 			struct symb *symb; */
			
/* 			owner_spd = services->spd; */
/* 			symb = &services->undef.symbs[i]; */
/* 			dest_service = symb->exporter; */

/* 			symb = symb->exported_symb; */
/* 			dest_spd = dest_service->spd; */
/* 			dest_entry_fn = symb->addr; */

/* 			if ((spd_add_static_cap(services->spd, dest_entry_fn, dest_spd, IL_ST) == 0)) { */
/* 				printl(PRINT_DEBUG, "Could not add capability for %s to %s.\n",  */
/* 				       symb->name, dest_service->obj); */
/* 			} */
/* 		} */

/* 		services = services->next; */
/* 	} */

/* 	return; */
/* } */

/* void start_composite(struct service_symbs *services) */
/* { */
/* 	struct thread *thd; */

/* 	spd_init(); */
/* 	ipc_init(); */
/* 	thd_init(); */

/* 	add_spds(services); */

/* 	thd = thd_alloc(services->spd); */

/* 	if (!thd) { */
/* 		printl(PRINT_DEBUG, "Could not allocate thread.\n"); */
/* 		return; */
/* 	} */

/* 	thd_set_current(thd); */

/* 	return; */
/* } */

#include "../module/aed_ioctl.h"

/*
 * FIXME: all the exit(-1) -> return NULL, and handling in calling
 * function.
 */
/*struct cap_info **/
int create_invocation_cap(struct spd_info *from_spd, struct service_symbs *from_obj, 
			  struct spd_info *to_spd, struct service_symbs *to_obj,
			  int cos_fd, char *client_fn, char *client_stub, 
			  char *server_stub, char *server_fn, int flags)
{
	struct cap_info cap;
	struct symb_type *st = &from_obj->undef;

	vaddr_t addr;
	int i;
	
	/* find in what position the symbol was inserted into the
	 * user-level capability table (which position was opted for
	 * use), so that we can insert the information into the
	 * correct user-capability. */
	for (i = 0 ; i < st->num_symbs ; i++) {
		if (strcmp(client_fn, st->symbs[i].name) == 0) {
			break;
		}
	}
	if (i == st->num_symbs) {
		printl(PRINT_DEBUG, "Could not find the undefined symbol %s in %s.\n", 
		       server_fn, from_obj->obj);
		exit(-1);
	}
	
	addr = (vaddr_t)get_symb_address(&to_obj->exported, server_stub);
	if (addr == 0) {
		printl(PRINT_DEBUG, "Could not find %s in %s.\n", server_stub, to_obj->obj);
		exit(-1);
	}
	cap.SD_serv_stub = addr;
	addr = (vaddr_t)get_symb_address(&from_obj->exported, client_stub);
	if (addr == 0) {
		printl(PRINT_DEBUG, "could not find %s in %s.\n", client_stub, from_obj->obj);
		exit(-1);
	}
	cap.SD_cli_stub = addr;
	addr = (vaddr_t)get_symb_address(&to_obj->exported, server_fn);
	if (addr == 0) {
		printl(PRINT_DEBUG, "could not find %s in %s.\n", server_fn, to_obj->obj);
		exit(-1);
	}
	cap.ST_serv_entry = addr;
	
	cap.rel_offset = i;
	cap.owner_spd_handle = from_spd->spd_handle;
	cap.dest_spd_handle = to_spd->spd_handle;
	cap.il = 3;
	cap.flags = flags;

	cap.cap_handle = cos_spd_add_cap(cos_fd, &cap);

 	if (cap.cap_handle == 0) {
		printl(PRINT_DEBUG, "Could not add capability # %d to %s (%d) for %s.\n", 
		       cap.rel_offset, from_obj->obj, cap.owner_spd_handle, server_fn);
		exit(-1);
	}
	
	return 0;
}

static struct symb *spd_contains_symb(struct service_symbs *s, char *name) 
{
	int i;
	struct symb_type *symbs = &s->exported; 

	for (i = 0 ; i < symbs->num_symbs ; i++) {
		if (strcmp(name, symbs->symbs[i].name) == 0) {
			return &symbs->symbs[i];
		}
	}
	return NULL;
}

static int create_spd_capabilities(struct service_symbs *service/*, struct spd_info *si*/, int cntl_fd)
{
	int i;
	struct symb_type *undef_symbs = &service->undef;
	struct spd_info *spd = (struct spd_info*)service->extern_info;
	
	for (i = 0 ; i < undef_symbs->num_symbs ; i++) {
		struct symb *symb = &undef_symbs->symbs[i];
		struct symb *exp_symb = symb->exported_symb;
		struct service_symbs *exporter = symb->exporter;
		struct spd_info *export_spd = (struct spd_info*)exporter->extern_info;
		struct symb *c_stub, *s_stub;
		char tmp[MAX_SYMB_LEN];

		if (MAX_SYMB_LEN-1 == snprintf(tmp, MAX_SYMB_LEN-1, "%s%s", symb->name, CAP_CLIENT_STUB_POSTPEND)) {
			printl(PRINT_HIGH, "symbol name %s too long to become client capability\n", symb->name);
			return -1;
		}
		c_stub = spd_contains_symb(service, tmp);
		if (NULL == c_stub) {
			c_stub = spd_contains_symb(service, CAP_CLIENT_STUB_DEFAULT);
			if (NULL == c_stub) {
				printl(PRINT_HIGH, "Could not find a client stub for function %s in service %s.\n",
				       symb->name, service->obj);
				return -1;
			}
		}

		if (MAX_SYMB_LEN-1 == snprintf(tmp, MAX_SYMB_LEN-1, "%s%s", exp_symb->name, CAP_SERVER_STUB_POSTPEND)) {
			printl(PRINT_HIGH, "symbol name %s too long to become server capability\n", exp_symb->name);
			return -1;
		}

		s_stub = spd_contains_symb(exporter, tmp);
		if (NULL == s_stub) {
			printl(PRINT_HIGH, "Could not find server stub (%s) for function %s in service %s to satisfy %s.\n",
			       tmp, exp_symb->name, exporter->obj, service->obj);
			return -1;
		}

		if (NULL == export_spd) {
			printl(PRINT_HIGH, "Trusted spd (spd_info) not attached to service symb yet.\n");
			return -1;
		}
		if (create_invocation_cap(spd, service, export_spd, exporter, cntl_fd, 
					  symb->name, c_stub->name, s_stub->name, 
					  exp_symb->name, 0)) {
			return -1;
		}
	}
	
	return 0;
}

struct spd_info *create_spd(int cos_fd, struct service_symbs *s, 
			    long lowest_addr, long size) 
{
	struct spd_info *spd;
	struct usr_inv_cap *ucap_tbl;
	vaddr_t upcall_addr, atomic_addr;
	long *spd_id_addr, *heap_ptr;
	int i;

	spd = (struct spd_info *)malloc(sizeof(struct spd_info));
	if (NULL == spd) {
		perror("Could not allocate memory for spd\n");
		return NULL;
	}
	
	ucap_tbl = (struct usr_inv_cap*)get_symb_address(&s->exported, 
							 USER_CAP_TBL_NAME);
	if (ucap_tbl == 0) {
		printl(PRINT_DEBUG, "Could not find a user capability tbl for %s.\n", s->obj);
		return NULL;
	}
	upcall_addr = (vaddr_t)get_symb_address(&s->exported, UPCALL_ENTRY_NAME);
	if (upcall_addr == 0) {
		printl(PRINT_DEBUG, "Could not find %s in %s.\n", UPCALL_ENTRY_NAME, s->obj);
		return NULL;
	}
	spd_id_addr = (long*)get_symb_address(&s->exported, SPD_ID_NAME);
	if (spd_id_addr == NULL) {
		printl(PRINT_DEBUG, "Could not find %s in %s.\n", SPD_ID_NAME, s->obj);
		return NULL;
	}
	heap_ptr = (long*)get_symb_address(&s->exported, HEAP_PTR);
	if (heap_ptr == NULL) {
		printl(PRINT_DEBUG, "Could not find %s in %s.\n", HEAP_PTR, s->obj);
		return NULL;
	}

	for (i = 0 ; i < NUM_ATOMIC_SYMBS ; i++) {
		atomic_addr = (vaddr_t)get_symb_address(&s->exported, ATOMIC_USER_DEF[i]);
		if (atomic_addr != 0) {
			spd->atomic_regions[i] = atomic_addr;
		} else {
			spd->atomic_regions[i] = 0;
		}
	}
	
	spd->num_caps = s->undef.num_symbs;
	spd->ucap_tbl = (vaddr_t)ucap_tbl;
	spd->lowest_addr = lowest_addr;
	spd->size = size;
	spd->upcall_entry = upcall_addr;

	spd->spd_handle = cos_create_spd(cos_fd, spd);
	if (spd->spd_handle < 0) {
		printl(PRINT_DEBUG, "Could not create spd %s\n", s->obj);
		free(spd);
		return NULL;
	}
	printl(PRINT_HIGH, "spd %s, id %d with initialization string \"%s\" @ %x.\n", 
	       s->obj, (unsigned int)spd->spd_handle, s->init_str, (unsigned int)spd->lowest_addr);
	*spd_id_addr = spd->spd_handle;
	printl(PRINT_DEBUG, "\tHeap pointer directed to %x.\n", (unsigned int)s->heap_top);
	*heap_ptr = s->heap_top;

	printl(PRINT_DEBUG, "\tFound ucap_tbl for component %s @ %p.\n", s->obj, ucap_tbl);
	printl(PRINT_DEBUG, "\tFound cos_upcall for component %s @ %p.\n", s->obj, (void*)upcall_addr);
	printl(PRINT_DEBUG, "\tFound spd_id address for component %s @ %p.\n", s->obj, spd_id_addr);
	for (i = 0 ; i < NUM_ATOMIC_SYMBS ; i++) {
		printl(PRINT_DEBUG, "\tFound %s address for component %s @ %x.\n", 
		       ATOMIC_USER_DEF[i], s->obj, (unsigned int)spd->atomic_regions[i]);
	}

	s->extern_info = spd;

	return spd;
}

void make_spd_scheduler(int cntl_fd, struct service_symbs *s, struct service_symbs *p)
{
	vaddr_t sched_page;
	struct spd_info *spd = s->extern_info, *parent = NULL;

	if (p) parent = p->extern_info;

	sched_page = (vaddr_t)get_symb_address(&s->exported, SCHED_PAGE_NAME);
	if (0 == sched_page) {
		printl(PRINT_DEBUG, "Could not find %s in %s.\n", SCHED_PAGE_NAME, s->obj);
		return;
	}
	printl(PRINT_DEBUG, "Found spd notification page @ %x.  Promoting to scheduler.\n", 
	       (unsigned int) sched_page);

	cos_promote_to_scheduler(cntl_fd, spd->spd_handle, (NULL == parent)? -1 : parent->spd_handle, sched_page);
	

	return;
}

/* Edge description of components.  Mirrored in mpd_mgr.c */
struct comp_graph {
	int client, server;
};

static int serialize_spd_graph(struct comp_graph *g, int sz, struct service_symbs *ss)
{
	struct comp_graph *edge;
	int g_frontier = 0;

	while (ss) {
		int i, cid, sid;

		assert(ss->extern_info);		
		cid = ((struct spd_info *)(ss->extern_info))->spd_handle;
		for (i = 0 ; i < ss->num_dependencies && 0 != cid ; i++) {
			struct service_symbs *dep = ss->dependencies[i].dep;
			assert(dep);

			sid = ((struct spd_info *)(dep->extern_info))->spd_handle;
			if (sid == 0) continue;
			if (g_frontier >= (sz-2)) {
				printl(PRINT_DEBUG, "More edges in component graph than can be serialized into the allocated region: fix cos_loader.c.\n");
				exit(-1);
			}

			edge = &g[g_frontier++];
			edge->client = cid;
			edge->server = sid;
			//printl(PRINT_DEBUG, "serialized edge @ %p: %d->%d.\n", edge, cid, sid);
		}
		
		ss = ss->next;
	}
	edge = &g[g_frontier];
	edge->client = edge->server = 0;

	return 0;
}

/* 
 * The only thing we need to do to the mpd manager is to let it know
 * the topology of the component graph.  Progress the heap pointer a
 * page, and serialize the component graph into that page.
 */
static void make_spd_mpd_mgr(struct service_symbs *mm, struct service_symbs *all)
{
	int **heap_ptr, *heap_ptr_val;
	struct comp_graph *g;

	heap_ptr = (int **)get_symb_address(&mm->exported, HEAP_PTR);
	if (heap_ptr == NULL) {
		printl(PRINT_DEBUG, "Could not find %s in %s.\n", HEAP_PTR, mm->obj);
		return;
	}
	heap_ptr_val = *heap_ptr;
	g = mmap((void*)heap_ptr_val, PAGE_SIZE, PROT_WRITE | PROT_READ,
			MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (MAP_FAILED == g){
		perror("Couldn't map the graph into the address space");
		return;
	}
	printl(PRINT_DEBUG, "Found mpd_mgr: remapping heap_ptr from %p to %p, serializing graph.\n",
	       *heap_ptr, heap_ptr_val + PAGE_SIZE/sizeof(heap_ptr_val));
	*heap_ptr = heap_ptr_val + PAGE_SIZE/sizeof(heap_ptr_val);

	serialize_spd_graph(g, PAGE_SIZE/sizeof(struct comp_graph), all);
}

#define INIT_STR_SZ 56

/* struct is 64 bytes, so we can have 64 entries in a page. */
struct component_init_str {
	unsigned int spdid, schedid;
	char init_str[INIT_STR_SZ];
};

static void format_config_info(struct service_symbs *ss, struct component_init_str *data)
{
	int i; 

	for (i = 0 ; ss ; i++, ss = ss->next) {
		char *info;
		int id;

		info = ss->init_str;
		if (strlen(info) >= INIT_STR_SZ) {
			printl(PRINT_HIGH, "Initialization string %s for component %s is too long (longer than %d)",
			       info, ss->obj, strlen(info));
			exit(-1);
		}
		id = ((struct spd_info *)(ss->extern_info))->spd_handle;
		
		data[i].spdid = id;
		data[i].schedid = ss->scheduler ? 
			((struct spd_info *)(ss->scheduler->extern_info))->spd_handle : 
			0;
		if (0 == strcmp(" ", info)) info = "";
		strcpy(data[i].init_str, info);
	}
	data[i].spdid = 0;
}

static void make_spd_config_comp(struct service_symbs *c, struct service_symbs *all)
{
	int **heap_ptr, *heap_ptr_val;
	struct component_init_str *info;

	heap_ptr = (int **)get_symb_address(&c->exported, HEAP_PTR);
	if (heap_ptr == NULL) {
		printl(PRINT_DEBUG, "Could not find %s in %s.\n", HEAP_PTR, c->obj);
		return;
	}
	heap_ptr_val = *heap_ptr;
	info = mmap((void*)heap_ptr_val, PAGE_SIZE, PROT_WRITE | PROT_READ,
			MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	if (MAP_FAILED == info){
		perror("Couldn't map the configuration info into the address space");
		return;
	}
	printl(PRINT_DEBUG, "Found %s: remapping heap_ptr from %p to %p, writing config info.\n",
	       CONFIG_COMP, *heap_ptr, heap_ptr_val + PAGE_SIZE/sizeof(heap_ptr_val));
	*heap_ptr = heap_ptr_val + PAGE_SIZE/sizeof(heap_ptr_val);

	format_config_info(all, info);
}

static struct service_symbs *find_obj_by_name(struct service_symbs *s, const char *n)
{
	while (s) {
		if (strstr(s->obj, n) != NULL) {
			return s;
		}

		s = s->next;
	}

	return NULL;
}

#define MAX_SCHEDULERS 3

static void setup_kernel(struct service_symbs *services)
{
	struct service_symbs *s;
	struct service_symbs *init = NULL;
	struct spd_info *init_spd = NULL;

	struct cos_thread_info thd;
	int cntl_fd, ret;
	int (*fn)(void);
	unsigned long long start, end;
	
	cntl_fd = aed_open_cntl_fd();

	s = services;
	while (s) {
		struct service_symbs *t;
		struct spd_info *t_spd;

		t = s;
		if (strstr(s->obj, INIT_COMP) != NULL) {
			init = t;
			t_spd = init_spd = create_spd(cntl_fd, init, 0, 0);
		} else {
			t_spd = create_spd(cntl_fd, t, t->lower_addr, t->size);
		}
		if (!t_spd) {
			fprintf(stderr, "\tCould not find service object.\n");
			exit(-1);
		}
		s = s->next;
	}

	s = services;
	while (s) {
		if (create_spd_capabilities(s, cntl_fd)) {
			fprintf(stderr, "\tCould not find all stubs.\n");
			exit(-1);
		}
		
		s = s->next;
	}
		printl(PRINT_DEBUG, "\n");

	if ((s = find_obj_by_name(services, ROOT_SCHED)) == NULL) {
		fprintf(stderr, "Could not find root scheduler\n");
		exit(-1);
	}
	make_spd_scheduler(cntl_fd, s, NULL);

/* 	if ((ds = find_obj_by_name(services, "d.o")) == NULL) { */
/* 		fprintf(stderr, "Could not find scheduler ds\n"); */
/* 		exit(-1); */
/* 	} */
/* 	make_spd_scheduler(cntl_fd, ds, s); */

//	cos_demo_spds(cntl_fd, spd3->spd_handle, spd4->spd_handle);
	thd.sched_handle = ((struct spd_info *)s->extern_info)->spd_handle;//spd2->spd_handle;

	if ((s = find_obj_by_name(services, MPD_MGR))) {
		make_spd_mpd_mgr(s, services);
	}

	if ((s = find_obj_by_name(services, CONFIG_COMP)) == NULL) {
		fprintf(stderr, "Could not find the configuration component.\n");
		exit(-1);
	}
	make_spd_config_comp(s, services);

	if ((s = find_obj_by_name(services, INIT_COMP)) == NULL) {
		fprintf(stderr, "Could not find initial component\n");
		exit(-1);
	}
	thd.spd_handle = ((struct spd_info *)s->extern_info)->spd_handle;//spd0->spd_handle;
	cos_create_thd(cntl_fd, &thd);

	printl(PRINT_HIGH, "\nOK, good to go, calling component 0's main\n\n");
	fflush(stdout);

	fn = (int (*)(void))get_symb_address(&s->exported, "spd0_main");

#define ITER 1
#define rdtscll(val) __asm__ __volatile__("rdtsc" : "=A" (val))

	aed_disable_syscalls(cntl_fd);
	rdtscll(start);
	ret = fn();
	rdtscll(end);
	aed_enable_syscalls(cntl_fd);

	printl(PRINT_HIGH, "Invocation takes %lld, ret %x.\n", (end-start)/ITER, ret);
	
	close(cntl_fd);

	return;
}

static inline void print_usage(int argc, char **argv)
{
	char *prog_name = argv[0];
	int i;

	printl(PRINT_HIGH, "Usage: %s <comma separated string of all "
	       "objs:truster1-trustee1|trustee2|...;truster2-...> "
	       "<path to gen_client_stub>\n",
	       prog_name);

	printl(PRINT_HIGH, "\nYou gave:");
	for (i = 0 ; i < argc ; i++) {
		printl(PRINT_HIGH, " %s", argv[i]);
	}
	printl(PRINT_HIGH, "\n");
	
	return;
}

#define STUB_PROG_LEN 128
extern vaddr_t SS_ipc_client_marshal;
extern vaddr_t DS_ipc_client_marshal;

//#define FAULT_SIGNAL
#ifdef FAULT_SIGNAL
#include <sys/ucontext.h>
void segv_handler(int signo, siginfo_t *si, void *context) {
	ucontext_t *uc = context;
	struct sigcontext *sc = (struct sigcontext *)&uc->uc_mcontext;

	printl(PRINT_HIGH, "Segfault: Faulting address %p, ip: %lx\n", si->si_addr, sc->eip);
	exit(-1);
}
#endif

#include <sched.h>
void set_prio(void)
{
	struct sched_param sp;

	if (sched_getparam(0, &sp) < 0) {
		perror("getparam: ");
		printl(PRINT_HIGH, "\n");
	}
	sp.sched_priority = 99;
	if (sched_setscheduler(0, SCHED_RR, &sp) < 0) {
		perror("setscheduler: "); printl(PRINT_HIGH, "\n");
	}

	return;
}

/*
 * Format of the input string is as such:
 * 
 * "s1,s2,s3,...,sn:s2-s3|...|sm;s3-si|...|sj"
 *
 * Where the pre-: comma-separated list is simply a list of all
 * involved services.  Post-: is a list of truster (before -) ->
 * trustee (all trustees separated by '|'s) relations separated by
 * ';'s.
 */
int main(int argc, char *argv[])
{
	struct service_symbs *services;
	char *delim = ":";
	char *servs, *dependencies, *stub_gen_prog;
	int ret = -1;

#ifdef FAULT_SIGNAL
	struct sigaction sa;

	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
#endif
	if (argc != 3) {
		print_usage(argc, argv);
		goto exit;
	}

	stub_gen_prog = argv[2];
#ifdef HIGHEST_PRIO
	set_prio();
#endif
	/* 
	 * NOTE: because strtok is used in prepare_service_symbs, we
	 * cannot use it relating to the command line args before AND
	 * after that invocation
	 */
	servs = strtok(argv[1], delim);
	dependencies = strtok(NULL, delim);

	if (!servs) {
		print_usage(argc, argv);
		goto exit;
	}

	bfd_init();

	services = prepare_service_symbs(servs);

	print_objs_symbs(services);

//	printl(PRINT_DEBUG, "Loading at %x:%d.\n", BASE_SERVICE_ADDRESS, DEFAULT_SERVICE_SIZE);

	if (!dependencies) {
		printl(PRINT_HIGH, "No dependencies given, not proceeding.\n");
		goto dealloc_exit;
	}
	
	if (deserialize_dependencies(dependencies, services)) {
		printl(PRINT_HIGH, "Error processing dependencies.\n");
		goto dealloc_exit;
	}

	if (verify_dependency_completeness(services)) {
		printl(PRINT_HIGH, "Unresolved symbols, not linking.\n");
		goto dealloc_exit;
	}

	if (verify_dependency_soundness(services)) {
		printl(PRINT_HIGH, "Services arranged in an invalid configuration, not linking.\n");
		goto dealloc_exit;
	}
	
	gen_stubs_and_link(stub_gen_prog, services);
	if (load_all_services(services)) {
		printl(PRINT_HIGH, "Error loading services, aborting.\n");
		goto dealloc_exit;
	}

//	print_kern_symbs(services);

	setup_kernel(services);

	ret = 0;

 dealloc_exit:
	while (services) {
		struct service_symbs *next = services->next;
		free_service_symbs(services);
		services = next;
	}
	/* FIXME: new goto label to dealloc spds */
 exit:
	return 0;
}
