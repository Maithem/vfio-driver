#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/vfio.h>
#include <unistd.h>
#include <stdint.h>

// Define the No-IOMMU type if it's missing from your headers
#ifndef VFIO_NOIOMMU_IOMMU
#define VFIO_NOIOMMU_IOMMU 8
#endif

int main() {
    int container, group, device;
    struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
    struct vfio_region_info reg = { .argsz = sizeof(reg) };

    container = open("/dev/vfio/vfio", O_RDWR);
    group = open("/dev/vfio/noiommu-0", O_RDWR);

    // 1. Attach group to container
    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);

    // 2. IMPORTANT: Set the IOMMU type to No-IOMMU (Type 8)
    // This is likely why it was failing with EINVAL
    if (ioctl(container, VFIO_SET_IOMMU, VFIO_NOIOMMU_IOMMU) < 0) {
        perror("Failed to set IOMMU type to No-IOMMU");
        return 1;
    }

    // 3. Get Device FD
    device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, "0000:00:03.0");
    if (device < 0) {
        perror("Getting device FD failed");
        return 1;
    }

    // 4. Map BAR0 (Registers)
    reg.index = 0;
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &reg);
    uint32_t *regs = mmap(NULL, reg.size, PROT_READ | PROT_WRITE, MAP_SHARED, device, reg.offset);

    printf("--- Hardware Access Successful ---\n");
    printf("EDU ID: 0x%08x\n", regs[0]);
    
    // Test Factorial: 5! = 120
    regs[2] = 5; 
    sleep(1);
    printf("Factorial Result: %u\n", regs[2]);

    return 0;
}
