# local_ip=$(ip addr show enp65s0f0np0 | grep 'inet ' | awk '{print $2}' | cut -d/ -f1)

sudo insmod rpage_allocator.ko
sudo insmod fastswap_rdma.ko sport=50000 sip="10.10.1.1" cip="10.10.1.2" nq=32
sudo insmod fastswap.ko

