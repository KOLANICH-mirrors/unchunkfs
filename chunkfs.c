/*
 *  ChunkFS - mount arbitrary files via FUSE as a tree of chunk files
 *  Copyright (C) 2007  Florian Zumbiehl <florz@florz.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 500
#define FUSE_USE_VERSION 25

#include <fuse.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <syslog.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

void die(bool,const char *,...) __attribute__ ((noreturn,format (printf,2,3)));

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

struct chunk_stat {
	int level;
	mode_t mode;
	off_t chunk,offset,size;
};

int image_fd;
off_t chunk_size,image_size,image_chunks;

#define min(a,b) (((a)<(b))?(a):(b))

#define cons_hexdig(a,b,c) { 		\
	uint64_t t;			\
	if((c)>='0'&&(c)<='9'){		\
		t=(c)-'0';		\
	}else if((c)>='a'&&(c)<='f'){	\
		t=(c)-'a'+10;		\
	}else{				\
		return -ENOENT;		\
	}				\
	a|=t<<(b);			\
}

int resolve_path(const char *path,struct chunk_stat *st){
	size_t l;
	uint64_t r;

	l=strlen(path);
	r=0;
	if(l<3||l>8*3||l%3){
		if(strcmp(path,"/"))return -ENOENT;
	}else{
		for(int x=0;x<l/3;x++){
			if(path[x*3]!='/')return -ENOENT;
			cons_hexdig(r,(8-x)*8-4,path[x*3+1]);
			cons_hexdig(r,(8-x)*8-8,path[x*3+2]);
		}
		if(r>=(uint64_t)image_chunks)return -ENOENT;
	}
	st->level=l/3;
	st->chunk=r;
	st->offset=st->chunk*chunk_size;
	if(st->level<8){
		st->mode=S_IFDIR;
	}else{
		st->mode=S_IFREG;
		st->size=min(image_size-st->offset,chunk_size);
	}
	return 0;
}

int chunkfs_getattr(const char *path,struct stat *buf){
	int r;
	struct chunk_stat st;

	if((r=resolve_path(path,&st))<0)return r;
	if(fstat(image_fd,buf)<0)return -errno;
	buf->st_mode=(buf->st_mode&~(S_IFMT|S_IXUSR|S_IXGRP|S_IXOTH))|(st.mode&S_IFMT);
	if(S_ISDIR(st.mode)){
		buf->st_mode=buf->st_mode
			|((buf->st_mode&S_IRUSR)?S_IXUSR:0)
			|((buf->st_mode&S_IRGRP)?S_IXGRP:0)
			|((buf->st_mode&S_IROTH)?S_IXOTH:0);
		buf->st_nlink=256+2;
		buf->st_size=0;
	}else{
		buf->st_nlink=1;
		buf->st_size=st.size;
	}
	buf->st_blocks=0;
	return 0;
}

int chunkfs_readdir(const char *path,void *buf,fuse_fill_dir_t filler,off_t offset,struct fuse_file_info *fi){
	int r;
	struct chunk_stat st;
	uint64_t chunks_per_entry;

	if((r=resolve_path(path,&st))<0)return r;
	if(!S_ISDIR(st.mode))return -ENOTDIR;
	chunks_per_entry=1ULL<<((8-1-st.level)*8);
	filler(buf,".",NULL,0);
	filler(buf,"..",NULL,0);
	for(uint64_t x=0;x<256&&(uint64_t)st.chunk+x*chunks_per_entry<(uint64_t)image_chunks;x++){
		char nbuf[3];
		sprintf(nbuf,"%02llx",x);
		filler(buf,nbuf,NULL,0);
	}
	return 0;
}

int chunkfs_open(const char *path,struct fuse_file_info *fi){
	int r;
	struct chunk_stat st;

	if((r=resolve_path(path,&st))<0)return r;
	if((fi->flags&O_ACCMODE)!=O_RDONLY)return -EROFS;
	return 0;
}

int chunkfs_read(const char *path,char *buf,size_t count,off_t offset,struct fuse_file_info *fi){
	int r;
	struct chunk_stat st;
	size_t r2;

	if((r=resolve_path(path,&st))<0)return r;
	if(S_ISDIR(st.mode))return -EISDIR;
	if(offset>st.size)return 0;
	count=min(st.size-offset,count);
	r2=pread(image_fd,buf,count,st.offset+offset);
	return r2<0?-errno:r2;
}

struct fuse_operations chunkfs_ops={
	.getattr=chunkfs_getattr,
	.readdir=chunkfs_readdir,
	.open=chunkfs_open,
	.read=chunkfs_read
};

int main(int argc,char **argv){
	struct stat st;
	char opt,*end;
	char *helpargv[2]={NULL,"-h"};

	openlog("chunkfs",LOG_CONS|LOG_NDELAY|LOG_PERROR|LOG_PID,LOG_DAEMON);
	opterr=0;
	while((opt=getopt(argc,argv,"ho:"))!=-1)
		if(opt=='h'){
			helpargv[0]=argv[0];
			return fuse_main(2,helpargv,NULL);
		}
	if(argc-optind!=3)die(false,"Usage: %s [options] <chunk size> <image file> <mount point>",*argv);
	errno=0;
	chunk_size=strtoll(argv[optind],&end,10);
	if(errno||*end||chunk_size<1)die(false,"Specified invalid chunk size");
	if((image_fd=open(argv[optind+1],O_RDONLY))<0)die(true,"open(image)");
	if(fstat(image_fd,&st)<0)die(true,"stat(image)");
	if(S_ISBLK(st.st_mode)){
		uint64_t blksize;
		if(ioctl(image_fd,BLKGETSIZE64,&blksize)<0)die(true,"ioctl(image,BLKGETSIZE64)");
		if(blksize>(uint64_t)INT64_MAX)die(false,"block device too large");
		image_size=blksize;
	}else{
		image_size=st.st_size;
	}
	image_chunks=image_size/chunk_size;
	if(image_size%chunk_size)image_chunks++;
	argv[optind]=argv[optind+2];
	argc-=2;
	return fuse_main(argc,argv,&chunkfs_ops);
}

