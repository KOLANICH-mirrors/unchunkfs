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
#include <inttypes.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include <fuse.h>

#include "utils.h"

static int chunkdir_fd;
static off_t chunk_size,image_size;

static int ensure_chunkdir_is_cwd(void){
	static bool done;

	if(!done){
		if(fchdir(chunkdir_fd)<0)return -errno;
		done=true;
	}
	return 0;
}

static int resolve_path(const char *path,mode_t *mode){
	if(!strcmp(path,"/")){
		*mode=S_IFDIR;
	}else if(!strcmp(path,"/image")){
		*mode=S_IFREG;
	}else{
		return -ENOENT;
	}
	return 0;
}

static void gen_chunk_name(char *buf,off_t num){
	sprintf(buf+7,"%016" PRIxMAX,(intmax_t)num);
	for(int x=0;x<7;x++){
		memmove(buf+x*3,buf+7+x*2,2);
		buf[x*3+2]='/';
	}
}

static int unchunkfs_getattr(const char *path,struct stat *buf){
	int r;
	mode_t mode;

	if((r=resolve_path(path,&mode))<0)return r;
	if(fstat(chunkdir_fd,buf)<0)return -errno;
	buf->st_mode=(buf->st_mode&~(S_IFMT|S_IXUSR|S_IXGRP|S_IXOTH))|(mode&S_IFMT);
	if(S_ISDIR(mode)){
		buf->st_mode=buf->st_mode
			|((buf->st_mode&S_IRUSR)?S_IXUSR:0)
			|((buf->st_mode&S_IRGRP)?S_IXGRP:0)
			|((buf->st_mode&S_IROTH)?S_IXOTH:0);
		buf->st_nlink=2;
		buf->st_size=0;
	}else{
		buf->st_nlink=1;
		buf->st_size=image_size;
	}
	buf->st_blocks=0;
	return 0;
}

static int unchunkfs_readdir(const char *path,void *buf,fuse_fill_dir_t filler,off_t offset,struct fuse_file_info *fi){
	int r;
	mode_t mode;

	if((r=resolve_path(path,&mode))<0)return r;
	if(!S_ISDIR(mode))return -ENOTDIR;
	filler(buf,".",NULL,0);
	filler(buf,"..",NULL,0);
	filler(buf,"image",NULL,0);
	return 0;
}

static int unchunkfs_open(const char *path,struct fuse_file_info *fi){
	int r;
	mode_t mode;

	if((r=resolve_path(path,&mode))<0)return r;
	if((fi->flags&O_ACCMODE)!=O_RDONLY)return -EROFS;
	return 0;
}

static int unchunkfs_read(const char *path,char *buf,size_t count,off_t offset,struct fuse_file_info *fi){
	int r,chunk_fd;
	mode_t mode;
	off_t chunk_num,chunk_offset;
	char chunk_name[24];
	ssize_t bufpos,r2;

	if(ensure_chunkdir_is_cwd()<0)return -EIO;
	if((r=resolve_path(path,&mode))<0)return r;
	if(S_ISDIR(mode))return -EISDIR;
	if(count>INT_MAX)die(false,"read request larger than can be represented in an int");
	if(count>SSIZE_MAX)die(false,"read request larger than can be represented in an ssize_t");
	count=min((off_t)count,max(image_size-offset,(off_t)0));
	for(bufpos=0;bufpos<count;){
		chunk_num=(offset+(off_t)bufpos)/chunk_size;
		chunk_offset=(offset+(off_t)bufpos)%chunk_size;
		gen_chunk_name(chunk_name,chunk_num);
		if((chunk_fd=open(chunk_name,O_RDONLY))<0)return -EIO;
		if((r2=pread(chunk_fd,buf+bufpos,count-bufpos,chunk_offset))>0)bufpos+=r2;
		if(close(chunk_fd)<0)die(true,"close(chunk_fd)");
		if(r2<=0)return -(r2?errno:EIO);
	}
	return bufpos;
}

static struct fuse_operations unchunkfs_ops={
	.getattr=unchunkfs_getattr,
	.readdir=unchunkfs_readdir,
	.open=unchunkfs_open,
	.read=unchunkfs_read
};

int main(int argc,char **argv){
	bool validation_error=false,show_version=false,show_help=false;
	int opt,ret=0;
	char *fake_argv[2]={"",NULL};
	int cwd_fd;
	struct stat st;
	off_t last_chunk,last_chunk_size;

	{
		int fd;

		if((fd=dup(0))<3||close(fd)<0)_exit(1);
	}

	openlog("unchunkfs",LOG_CONS|LOG_NDELAY|LOG_PERROR|LOG_PID,LOG_DAEMON);
	while((opt=getopt(argc,argv,":hVdfso:"))!=-1)
		switch(opt){
			case 'h':
				show_help=true;
				break;
			case 'V':
				show_version=true;
				break;
			case '?':
				fprintf(stderr,"unchunkfs: invalid option: -%c\n",optopt);
				validation_error=true;
				break;
			case ':':
				fprintf(stderr,"unchunkfs: option requires an argument: -%c\n",optopt);
				validation_error=true;
				break;
		}
	if(show_help+show_version>1||argc-optind!=(show_help||show_version?0:2))validation_error=true;
	if(validation_error||show_help){
		fprintf(stderr,
			"Usage: %s [options] <chunk dir> <mount point>\n"
			"\n"
			"general options:\n"
			"    -o opt[,opt...]        mount options\n"
			"    -h                     print help\n"
			"    -V                     print version\n"
			"\n",
			*argv);
		fake_argv[1]="-ho";
		fuse_main(2,fake_argv,&unchunkfs_ops);
		ret=validation_error;
	}else if(show_version){
		fprintf(stderr,"UnChunkFS v" VERSION "\n");
		fake_argv[1]="-V";
		fuse_main(2,fake_argv,&unchunkfs_ops);
	}else{
		if((cwd_fd=open(".",O_RDONLY))<0)die(true,"open(.)");
		if((chunkdir_fd=open(argv[optind],O_RDONLY))<0)die(true,"open(<chunk dir>)");
		if(fchdir(chunkdir_fd)<0)die(true,"fchdir(<chunk dir>)");
		last_chunk=0;
		last_chunk_size=0;
		for(int x=64;x--;){
			off_t new_last_chunk;
			char chunk_name[24];

			new_last_chunk=last_chunk|(x<63?(off_t)1<<x:0);
			gen_chunk_name(chunk_name,new_last_chunk);
			if(stat(chunk_name,&st)<0){
				if(errno!=ENOENT)die(true,"stat(<chunk dir>/%s)",chunk_name);
			}else{
				if(st.st_size<1)die(false,"<chunk dir>/%s is smaller than one byte",chunk_name);
				if(!chunk_size)chunk_size=st.st_size;
				last_chunk=new_last_chunk;
				last_chunk_size=st.st_size;
			}
		}
		if(chunk_size<1)chunk_size=1;
		if(last_chunk>ANYSINT_MAX(off_t)/chunk_size)die(false,"the sum of the chunks is too large");
		image_size=last_chunk*chunk_size;
		if(last_chunk_size>ANYSINT_MAX(off_t)-image_size)die(false,"the sum of the chunks is too large");
		image_size+=last_chunk_size;
		argv[optind]=argv[optind+1];
		argc--;
		if(fchdir(cwd_fd)<0)die(true,"fchdir(<start cwd>)");
		ret=fuse_main(argc,argv,&unchunkfs_ops);
	}
	return ret;
}

