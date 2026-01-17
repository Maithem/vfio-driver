#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/vfio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <stdint.h>

/**
 * Helper: Find the IOMMU group ID from sysfs
 */
int get_group_id(const char *pci_addr) {
    char path[PATH_MAX];
    char link[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", pci_addr);
    
    ssize_t len = readlink(path, link, sizeof(link) - 1);
    if (len == -1) {
        perror("readlink (Is the device bound to vfio-pci?)");
        return -1;
    }
    link[len] = '\0';
    return atoi(basename(link));
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <pci_address>\n", argv[0]);
        return 1;
    }

    const char *pci_addr = argv[1];
    int group_id = get_group_id(pci_addr);
    if (group_id < 0) return 1;

    int container, group, device;
    char group_path[64];

    // 1. Setup the Container
    container = open("/dev/vfio/vfio", O_RDWR);
    if (ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        fprintf(stderr, "Unknown VFIO API version\n");
        return 1;
    }

    // 2. Open Group and attach to container
    snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", group_id);
    group = open(group_path, O_RDWR);
    if (group < 0) {
        perror("Failed to open group");
        return 1;
    }
    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);

    // 3. Set IOMMU type (VFIO_TYPE1_IOMMU is standard for x86)
    if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) {
        perror("Failed to set IOMMU type");
        return 1;
    }

    // 4. Get the Device File Descriptor
    device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, pci_addr);
    if (device < 0) {
        perror("Failed to get device FD");
        return 1;
    }

    // 5. Query Identification
    struct vfio_region_info config_reg = { .argsz = sizeof(config_reg), .index = VFIO_PCI_CONFIG_REGION_INDEX };
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &config_reg);

    uint16_t vendor_id, device_id;
    pread(device, &vendor_id, 2, config_reg.offset + 0);
    pread(device, &device_id, 2, config_reg.offset + 2);

    printf("\n=== Device Identification ===\n");
    printf("Vendor ID: 0x%04X (VMware)\n", vendor_id);
    printf("Device ID: 0x%04X (VMXNET3 Virtual Ethernet)\n", device_id);

    // 6. Query and Map BAR0 (Registers)
    struct vfio_region_info bar0_reg = { .argsz = sizeof(bar0_reg), .index = 0 };
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar0_reg);

    void *bar0_ptr = mmap(NULL, bar0_reg.size, PROT_READ | PROT_WRITE, 
                          MAP_SHARED, device, bar0_reg.offset);

    if (bar0_ptr == MAP_FAILED) {
        perror("mmap BAR0 failed");
    } else {
        printf("\n=== Register Access ===\n");
        printf("BAR0 Size: %llu bytes\n", (unsigned long long)bar0_reg.size);
        printf("BAR0 Mapped at: %p\n", bar0_ptr);
    }

    // 7. DMA Setup: Allocate and Map memory for the hardware
    size_t dma_size = 4096 * 4; // Allocate 4 pages (16KB)
    void *dma_vaddr = mmap(NULL, dma_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);

    if (dma_vaddr == MAP_FAILED) {
        perror("DMA mmap failed");
    } else {
        struct vfio_iommu_type1_dma_map dma_map = {
            .argsz = sizeof(dma_map),
            .vaddr = (uintptr_t)dma_vaddr,
            .size  = dma_size,
            .iova  = 0x1000000, // IO Virtual Address starts at 16MB
            .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        };

        if (ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
            perror("VFIO_IOMMU_MAP_DMA failed");
        } else {
            printf("\n=== DMA Setup ===\n");
            printf("DMA Buffer Allocated: %zu bytes\n", dma_size);
            printf("Userspace Vaddr:      %p\n", dma_vaddr);
            printf("Device IOVA:          0x%llx\n", dma_map.iova);
        }
    }

    // Keep the program running for a moment so you can inspect /proc/self/maps
    printf("\nPress Enter to exit and cleanup...");
    getchar();

    // Cleanup
    if (dma_vaddr != MAP_FAILED) munmap(dma_vaddr, dma_size);
    if (bar0_ptr != MAP_FAILED) munmap(bar0_ptr, bar0_reg.size);
    close(device);
    close(group);
    close(container);
    
    return 0;
}
