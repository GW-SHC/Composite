#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <cos_component.h>
#include <cos_kernel_api.h>
#include <cobj_format.h>

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

void panic(char* message){
	printc("cFE panic: %s", message);
	assert(0);
}

void __isoc99_sscanf(void){
	panic("__isoc99_sscanf not implemented!");
}