#!/bin/bash

echo "hostname 확인"
target=$(hostname)
echo $1
if [ -z "$1" ]
then
	echo "$1 is null, exit"
	exit
fi


if [ ! -f $1 ]
then
	echo "no file";
	exit;
fi

if [ $target != "raspberrypi" ]
then
	exit
else

	if [ -d ./newImage ]
	then
		echo "newImage 삭제"
		rm ./newImage -rf
	fi
	#if [ ! -d ./newImage/boot ]
	#then
	#	echo "directory make"
	#	mkdir ./newImage/boot
	#fi
	
	echo "tar.gz 풀기"
	tar -xzf $1

	echo "권한 일괄 수정"
	chown root:root -R ./newImage


	echo "boot 복사"
	cp -a ./newImage/boot/* /boot

	echo "module 복사"
	cp -a ./newImage/rootfs/lib/modules/ /lib/modules
fi
