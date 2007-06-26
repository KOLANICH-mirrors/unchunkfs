
#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <stdlib.h>

#include "utils.h"

void die(bool syserr,const char *fmt,...){
	va_list ap;
	char buf[1024];

	va_start(ap,fmt);
	vsnprintf(buf,sizeof(buf),fmt,ap);
	buf[sizeof(buf)-1]=0;
	if(syserr)syslog(LOG_ERR,"%s: %m, exiting",buf);
		else syslog(LOG_ERR,"%s, exiting",buf);
	exit(1);
}

