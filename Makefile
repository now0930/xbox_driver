obj-m = xbox_test.o


KVERSION = $(shell uname -r)
all:
	#make -C /lib/modules/$(KVERSION)/build V=1 M=$(PWD) modules
	make -C /lib/modules/`uname -r`/build M=`pwd` modules
clean:
	test ! -d /lib/modules/$(KVERSION) || make -C /lib/modules/$(KVERSION)/build V=1 M=$(PWD) clean
