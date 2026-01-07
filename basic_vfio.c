#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/vfio.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    int container, group, device;
    struct vfio_group_status group_status = { .argsz = sizeof(group_status) };
    struct vfio_device_info device_info = { .argsz = sizeof(device_info) };

    // 1. Setup the Container (The memory sandbox)
    container = open("/dev/vfio/vfio", O_RDWR);
    if (ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        fprintf(stderr, "Unknown VFIO API version\n");
        return 1;
    }

    // 2. Open Group 19 and attach to container
    group = open("/dev/vfio/19", O_RDWR);
    if (group < 0) {
        perror("Failed to open /dev/vfio/19");
        return 1;
    }
    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);

    // 3. Set IOMMU type (enables DMA protection)
    if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
        perror("Failed to set IOMMU type");
        return 1;
    }

    // 4. Get the actual Device File Descriptor
    device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, "0000:02:00.0");
    if (device < 0) {
        perror("Failed to get device FD");
        return 1;
    }

    // 5. Query Make and Model from PCI Config Space (Region 6)
    struct vfio_region_info config_reg = { .argsz = sizeof(config_reg), .index = VFIO_PCI_CONFIG_REGION_INDEX };
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &config_reg);

    unsigned short vendor_id, device_id;
    pread(device, &vendor_id, 2, config_reg.offset + 0);
    pread(device, &device_id, 2, config_reg.offset + 2);

    printf("\n=== Device Identification ===\n");
    printf("Vendor ID: 0x%04X (VMware)\n", vendor_id);
    printf("Device ID: 0x%04X (VMXNET3 Virtual Ethernet)\n", device_id);

    // 6. Query and Map BAR0 (Region 0) - Hardware Registers
    struct vfio_region_info bar0_reg = { .argsz = sizeof(bar0_reg), .index = 0 };
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar0_reg);

    void *bar0_ptr = mmap(NULL, bar0_reg.size, PROT_READ | PROT_WRITE, 
                          MAP_SHARED, device, bar0_reg.offset);

    if (bar0_ptr == MAP_FAILED) {
        perror("mmap BAR0 failed");
    } else {
        printf("\n=== Register Access ===\n");
        printf("BAR0 Size: %llu bytes\n", bar0_reg.size);
        printf("BAR0 Mapped at: %p\n", bar0_ptr);
    }

    close(device);
    close(group);
    close(container);
    return 0;
}
