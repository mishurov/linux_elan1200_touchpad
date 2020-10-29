obj-m = hid-elan1200.o
KVERSION = $(shell uname -r)
all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules CFLAGS_hid-elan1200.o=$(CFLAGS)
clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
