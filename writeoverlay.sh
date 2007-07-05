#!/bin/sh
#
# Copyright (C) 2007  Florian Zumbiehl <florz@florz.de>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

#
# !!! Be warned that this script isn't very sophisticated in terms of     !!!
# !!! error handling and the handling of special characters in arguments! !!!
#
# This script allows you to create from an arbitrary block device a
# writable clone by the means of the Linux device mapper's snapshot
# facilities. Any writes to the clone will be stored in a temporary file.
#
# The primary intended use for this is to allow for read-only filesystem
# images (like from backups) to be mounted, even if they need some form
# of recovery, like a log replay in case of a journalling filesystem.
# That's a common problem if the image was created from a block device
# snapshot of a live filesystem.
#
# If the image is a regular file, you'll first have to create a loop
# device from that, and then use this script on that loop device.
#

set -e
#set -x

usage(){
	echo usage: $0 '[ create <origdev> <cowsize> <cow_sect_per_chunk> <mapper_dev_name> | remove <mapper_dev_name> ]'
	exit 1
}

case "$1" in
	create)
		[ $# -eq 5 ] || usage
		ORIGDEV=$2
		COWSIZE=$3
		SECT_PER_CHUNK=$4
		OVERLAYDEV=$5
		COWFILE=`mktemp -t writeoverlay.XXXXXXXXXX`
		dd if=/dev/zero bs=1 seek=$COWSIZE count=1 of=$COWFILE > /dev/null
		LOOPDEV=`losetup -f`
		losetup $LOOPDEV $COWFILE
		rm $COWFILE
		echo "0 `blockdev --getsz $ORIGDEV` snapshot $ORIGDEV $LOOPDEV n $SECT_PER_CHUNK" | dmsetup create $OVERLAYDEV
		;;
	remove)
		[ $# -eq 2 ] || usage
		OVERLAYDEV=$2
		LOOPDEV=/dev/loop`dmsetup table $OVERLAYDEV | cut '-d ' -f5 | cut -d: -f2`
		dmsetup remove $OVERLAYDEV
		losetup -d $LOOPDEV
		;;
	*)
		usage
		;;
esac

