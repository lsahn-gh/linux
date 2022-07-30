sudo qemu-system-aarch64 				\
	-M virt 						\
	-dtb ./abc.dtb				\
	-cpu cortex-a72  						\
	-nographic 						\
	-smp 3 							\
	-m 4096							\
	-kernel //home/kkr/main/iamroot/qemu/img/dma_Image 	\
	-append "rootwait root=/dev/vda rw console=ttyAMA0" 	\
	-device virtio-net-device,netdev=mynet1 \
	-netdev tap,id=mynet1,ifname=tap0,script=no,downscript=no \
	-device virtio-blk-device,drive=disk \
	-drive if=none,id=disk,file=/dev/sdd,format=raw

#-M virt,dumpdtb=abc.dtb 						\
#if=none,format=raw,id=disk 				\
#-device virtio-net-device,netdev=eth0  			\
#-device virtio-blk-device,drive=disk
#-drive file=/home/kkr/main/iamroot/qemu/rfs/buildroot/output/images/rootfs.ext4,
#if=none,format=raw,id=hd0 				
#-device virtio-blk-device,drive=hd0
#-enable-kvm 					
