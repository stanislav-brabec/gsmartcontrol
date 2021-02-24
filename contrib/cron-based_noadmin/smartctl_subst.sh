#!/bin/bash
############################################################################
# Copyright:
#      (C) 2008 - 2021  Alex Butcher <alex dot butcher 'at' assursys.co.uk>
#      (C) 2008 - 2021  Alexander Shaduri <ashaduri 'at' gmail.com>
# License: BSD Zero Clause License file
############################################################################

if [ "$1" = "-V" ]; then
	smartctl -V 2>&1
	exit
fi

while [ $# -ge 1 ]; do
	device="$1"
	shift
done

if [ "$device" = "" ]; then
	echo "Usage: $0 <device>"
	exit 1;
fi

dev_base="`basename \"$device\"`"
out_file=/var/run/smart-"$dev_base"

cat "$out_file"
