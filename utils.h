
#include <limits.h>
#include <stdbool.h>

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))
#define ANYSINT_MAX(t) ((((t)1<<(sizeof(t)*CHAR_BIT-2))-(t)1)*(t)2+(t)1)

void die(bool,const char *,...) __attribute__ ((noreturn,format (printf,2,3)));

