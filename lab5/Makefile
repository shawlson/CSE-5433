obj-m += quickfs.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	gcc mkquickfs.c -lrt -o mkquickfs

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm mkquickfs

