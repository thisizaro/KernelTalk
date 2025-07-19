# KernelTalk Makefile
# Builds both kernel module and user-space client

# Kernel module build
obj-m += kerneltalk_mod.o

# Get kernel build directory
KERNEL_DIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Default target
all: module client

# Build kernel module
module:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

# Build user-space client
client:
	gcc -o kerneltalk_client kerneltalk_client.c

# Clean build artifacts
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f kerneltalk_client

# Install module (requires root)
install: module
	sudo insmod kerneltalk_mod.ko
	sudo mknod /dev/kerneltalk c $$(grep kerneltalk /proc/devices | cut -d' ' -f1) 0
	sudo chmod 666 /dev/kerneltalk

# Remove module
uninstall:
	sudo rmmod kerneltalk_mod
	sudo rm -f /dev/kerneltalk

# Help target
help:
	@echo "KernelTalk Build System"
	@echo "Available targets:"
	@echo "  all        - Build both kernel module and client"
	@echo "  module     - Build kernel module only"
	@echo "  client     - Build user-space client only"
	@echo "  clean      - Clean build artifacts"
	@echo "  install    - Install module and create device node"
	@echo "  uninstall  - Remove module and device node"
	@echo "  help       - Show this help message"

.PHONY: all module client clean install uninstall help
