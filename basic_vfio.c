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
#include <string.h>

/* VMXNET3 Constants */
#define VMXNET3_REG_VRRS  0x000  // Version Report Selection
#define VMXNET3_REG_CMD   0x010  // Command Register
#define VMXNET3_REG_MACL  0x018  // MAC Address Low
#define VMXNET3_REG_MACH  0x020  // MAC Address High

#define VMXNET3_CMD_FIRST_SET    0xCF000000
#define VMXNET3_CMD_ACTIVATE_DEV (VMXNET3_CMD_FIRST_SET + 0)
#define VMXNET3_CMD_RESET_DEV    (VMXNET3_CMD_FIRST_SET + 1)
#define VMXNET3_CMD_GET_MAC      (VMXNET3_CMD_FIRST_SET + 4)

#define RING_SIZE 128

/* Structs must be packed for hardware compatibility */
typedef struct Vmxnet3_TxDesc {
    uint64_t addr;
    uint32_t len : 14, gen : 1, res1 : 17;
    uint32_t res2;
} __attribute__((__packed__)) Vmxnet3_TxDesc;

typedef struct Vmxnet3_RxDesc {
    uint64_t addr;
    uint32_t len : 14, btype : 1, gen : 1, res1 : 16;
    uint32_t res2;
} __attribute__((__packed__)) Vmxnet3_RxDesc;

// Simplified Driver Shared structure for activation
typedef struct Vmxnet3_DriverShared {
    uint32_t magic;
    uint32_t pad1;
    uint64_t dev_addr;    // IOVA of this struct
    uint64_t diag_addr;
    uint32_t vcpu_conf_addr;
    uint32_t vcpu_conf_len;
    // In a real driver, much more goes here (interrupts, ring locations)
} __attribute__((__packed__)) Vmxnet3_DriverShared;

int get_group_id(const char *pci_addr) {
    char path[PATH_MAX], link[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", pci_addr);
    ssize_t len = readlink(path, link, sizeof(link) - 1);
    if (len == -1) return -1;
    link[len] = '\0';
    return atoi(basename(link));
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: %s <pci_address>\n", argv[0]); return 1; }
    const char *pci_addr = argv[1];

    printf("[1] Locating IOMMU group for %s...\n", pci_addr);
    int group_id = get_group_id(pci_addr);
    if (group_id < 0) { perror("Readlink failed"); return 1; }

    printf("[2] Initializing VFIO Container and Group %d...\n", group_id);
    int container = open("/dev/vfio/vfio", O_RDWR);
    int group = open("/dev/vfio/19", O_RDWR); // Placeholder, replaced below
    char group_path[32];
    snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", group_id);
    group = open(group_path, O_RDWR);
    
    ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);
    ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
    int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, pci_addr);

    printf("[3] Mapping BAR0 Registers...\n");
    struct vfio_region_info bar0_reg = { .argsz = sizeof(bar0_reg), .index = 0 };
    ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &bar0_reg);
    uint8_t *bar0 = mmap(NULL, bar0_reg.size, PROT_READ|PROT_WRITE, MAP_SHARED, device, bar0_reg.offset);

    printf("[4] Allocating 64KB DMA Buffer and Mapping IOMMU...\n");
    size_t dma_size = 64 * 1024;
    void *vaddr = mmap(NULL, dma_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_LOCKED, -1, 0);
    uint64_t iova = 0x1000000;
    struct vfio_iommu_type1_dma_map dma_map = {
        .argsz = sizeof(dma_map), .vaddr = (uintptr_t)vaddr, .size = dma_size,
        .iova = iova, .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE
    };
    ioctl(container, VFIO_IOMMU_MAP_DMA, &dma_map);

    printf("[5] Partitioning Memory: SharedData, TxRing, RxRing...\n");
    Vmxnet3_DriverShared *shared = (Vmxnet3_DriverShared *)vaddr;
    Vmxnet3_TxDesc *tx_ring = (Vmxnet3_TxDesc *)((uint8_t*)vaddr + 1024);
    Vmxnet3_RxDesc *rx_ring = (Vmxnet3_RxDesc *)((uint8_t*)tx_ring + (RING_SIZE * sizeof(Vmxnet3_TxDesc)));
    
    memset(vaddr, 0, dma_size);
    shared->magic = 0x544D4E58; // "XNMT" Magic
    shared->dev_addr = iova;    // Tell device where this struct is (IOVA)

    printf("[6] Resetting VMXNET3 Device...\n");
    *(volatile uint32_t *)(bar0 + VMXNET3_REG_CMD) = VMXNET3_CMD_RESET_DEV;

    printf("[7] Retrieving MAC Address...\n");
    *(volatile uint32_t *)(bar0 + VMXNET3_REG_CMD) = VMXNET3_CMD_GET_MAC;
    uint32_t mac_low = *(volatile uint32_t *)(bar0 + VMXNET3_REG_MACL);
    uint32_t mac_high = *(volatile uint32_t *)(bar0 + VMXNET3_REG_MACH);
    printf("    MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac_low & 0xFF, (mac_low >> 8) & 0xFF, (mac_low >> 16) & 0xFF,
           (mac_low >> 24) & 0xFF, mac_high & 0xFF, (mac_high >> 8) & 0xFF);

    printf("[8] Activating Device (Linking SharedData IOVA)...\n");
    // To activate, we write the IOVA of the DriverShared struct to the BAR
    *(volatile uint32_t *)(bar0 + 0x28) = (uint32_t)iova;        // Low 32 bits
    *(volatile uint32_t *)(bar0 + 0x30) = (uint32_t)(iova >> 32); // High 32 bits
    *(volatile uint32_t *)(bar0 + VMXNET3_REG_CMD) = VMXNET3_CMD_ACTIVATE_DEV;

    uint32_t status = *(volatile uint32_t *)(bar0 + VMXNET3_REG_CMD);
    if (status == 0) printf("    Activation Successful!\n");
    else printf("    Activation status: 0x%X\n", status);

    printf("\nDevice is LIVE. Press Enter to shutdown...\n");
    getchar();

    printf("[9] Cleaning up...\n");
    *(volatile uint32_t *)(bar0 + VMXNET3_REG_CMD) = VMXNET3_CMD_RESET_DEV;
    munmap(vaddr, dma_size);
    munmap(bar0, bar0_reg.size);
    close(device); close(group); close(container);
    return 0;
}
