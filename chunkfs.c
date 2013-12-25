/*
 *  ChunkFS - mount arbitrary files via FUSE as a tree of chunk files
 *  Copyright (C) 2007-2013  Florian Zumbiehl <florz@florz.de>
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
#include <sys/ioctl.h>
#include <linux/fs.h>

#include <fuse.h>

#include "utils.h"

struct chunk_stat {
	int level;
	mode_t mode;
	off_t chunk,offset,size;
};

static int image_fd;
static off_t chunk_size,image_size,image_chunks;

static int resolve_path(const char *path,struct chunk_stat *st){
	size_t l;
	uint64_t r;
	uint8_t tmp;

	l=strlen(path);
	r=0;
	if(l<3||l>8*3||l%3){
		if(strcmp(path,"/"))return -ENOENT;
	}else{
		for(int x=0;x<l;x++){
			tmp=path[x];
			if(x%3){
				if(tmp>='0'&&tmp<='9'){
					tmp-='0';
				}else if(tmp>='a'&&tmp<='f'){
					tmp-='a'-10;
				}else{
					return -ENOENT;
				}
				r|=((uint64_t)tmp)<<((8*2-x/3*2-x%3)*4);
			}else if(tmp!='/')return -ENOENT;
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

static int chunkfs_getattr(const char *path,struct stat *buf){
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

static int chunkfs_readdir(const char *path,void *buf,fuse_fill_dir_t filler,off_t offset,struct fuse_file_info *fi){
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

static int chunkfs_open(const char *path,struct fuse_file_info *fi){
	int r;
	struct chunk_stat st;

	if((r=resolve_path(path,&st))<0)return r;
	if((fi->flags&O_ACCMODE)!=O_RDONLY)return -EROFS;
	return 0;
}

static int chunkfs_read(const char *path,char *buf,size_t count,off_t offset,struct fuse_file_info *fi){
	int r;
	struct chunk_stat st;
	ssize_t bufpos,r2;

	if((r=resolve_path(path,&st))<0)return r;
	if(S_ISDIR(st.mode))return -EISDIR;
	if(count>INT_MAX)die(false,"read request larger than can be represented in an int");
	if(count>SSIZE_MAX)die(false,"read request larger than can be represented in an ssize_t");
	count=min((off_t)count,max(st.size-offset,(off_t)0));
	for(bufpos=0;bufpos<count;){
		if((r2=pread(image_fd,buf+bufpos,count-bufpos,st.offset+offset+(off_t)bufpos))<0)return -errno;
		if(!r2)return -EIO;
		bufpos+=r2;
	}
	return bufpos;
}

static struct fuse_operations chunkfs_ops={
	.getattr=chunkfs_getattr,
	.readdir=chunkfs_readdir,
	.open=chunkfs_open,
	.read=chunkfs_read
};

int main(int argc,char **argv){
	bool validation_error=false,show_version=false,show_help=false;
	int opt,ret=0;
	char *fake_argv[2]={"",NULL};
	char *end;
	struct stat st;

	{
		int fd;

		if((fd=dup(0))<3||close(fd)<0)_exit(1);
	}

	openlog("chunkfs",LOG_CONS|LOG_NDELAY|LOG_PERROR|LOG_PID,LOG_DAEMON);
	while((opt=getopt(argc,argv,":hVdfso:"))!=-1)
		switch(opt){
			case 'h':
				show_help=true;
				break;
			case 'V':
				show_version=true;
				break;
			case '?':
				fprintf(stderr,"chunkfs: invalid option: -%c\n",optopt);
				validation_error=true;
				break;
			case ':':
				fprintf(stderr,"chunkfs: option requires an argument: -%c\n",optopt);
				validation_error=true;
				break;
		}
	if(show_help+show_version>1||argc-optind!=(show_help||show_version?0:3))validation_error=true;
	if(validation_error||show_help){
		fprintf(stderr,
			"Usage: %s [options] <chunk size> <image file> <mount point>\n"
			"\n"
			"general options:\n"
			"    -o opt[,opt...]        mount options\n"
			"    -h                     print help\n"
			"    -V                     print version\n"
			"\n",
			*argv);
		fake_argv[1]="-ho";
		fuse_main(2,fake_argv,&chunkfs_ops);
		ret=validation_error;
	}else if(show_version){
		fprintf(stderr,"ChunkFS v" VERSION "\n");
		fake_argv[1]="-V";
		fuse_main(2,fake_argv,&chunkfs_ops);
	}else{
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
		ret=fuse_main(argc,argv,&chunkfs_ops);
	}
	return ret;
}

