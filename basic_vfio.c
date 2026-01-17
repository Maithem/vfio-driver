#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/vfio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>

/**
 * Helper: Find the IOMMU group ID by reading the symlink in sysfs
 * Path: /sys/bus/pci/devices/<addr>/iommu_group -> ../../../kernel/iommu_groups/<id>
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
    
    // basename() gets the last part of the path (the ID)
    return atoi(basename(link));
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <pci_address>\n", argv[0]);
        printf("Example: %s 0000:02:00.0\n", argv[0]);
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

    // 2. Open the discovered Group and attach to container
    snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", group_id);
    group = open(group_path, O_RDWR);
    if (group < 0) {
        perror("Failed to open group");
        return 1;
    }
    
    if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
        perror("Failed to set container");
        return 1;
    }

    // 3. Set IOMMU type (Type 1 is standard for x86)
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

    // 5. Query Identification from PCI Config Space
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
        printf("BAR0 Size: %llu bytes\n", (unsigned long long)bar0_reg.size);
        printf("BAR0 Mapped at: %p\n", bar0_ptr);
    }

    // Cleanup
    if (bar0_ptr != MAP_FAILED) munmap(bar0_ptr, bar0_reg.size);
    close(device);
    close(group);
    close(container);
    
    return 0;
}
