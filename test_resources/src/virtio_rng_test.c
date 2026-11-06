#include "io.h"
#include "log.h"
#include "virtio/virtio_mmio.h"
#include "virtio/virtio_queue.h"
#include <stdint.h>

#define QUEUE_SIZE 8
#define AVAIL_RING_SIZE (sizeof(uint16_t) * QUEUE_SIZE + sizeof(VirtQueueAvail))
#define USED_RING_SIZE (sizeof(VirtQueueUsedElem) * QUEUE_SIZE + sizeof(VirtQueueUsed))
#define DESC_SIZE 8
#define RANDOM_SIZE 256

struct VirtIOMMIOLayout {
    uint32_t magic_value;           // 0x000
    uint32_t version;               // 0x004
    uint32_t device_id;             // 0x008
    uint32_t vendor_id;             // 0x00c
    uint32_t device_features;       // 0x010
    uint32_t device_features_sel;   // 0x014
    uint32_t reserved_0[2];         // 0x018, 0x01c
    uint32_t driver_features;       // 0x020
    uint32_t driver_features_sel;   // 0x024
    uint32_t reserved_1[2];         // 0x028, 0x02c
    uint32_t queue_sel;             // 0x030
    uint32_t queue_num_max;         // 0x034
    uint32_t queue_num;             // 0x038
    uint32_t queue_align;           // 0x03c
    uint32_t queue_pfn;             // 0x040
    uint32_t queue_ready;           // 0x044
    uint32_t reserved_2[2];         // 0x048, 0x04c
    uint32_t queue_notify;          // 0x050
    uint32_t reserved_3[3];         // 0x054, 0x058, 0x05c
    uint32_t interrupt_status;      // 0x060
    uint32_t interrupt_ack;         // 0x064
    uint32_t reserved_4[2];         // 0x068, 0x06c
    uint32_t status;                // 0x070
    uint32_t reserved_5[3];         // 0x074, 0x078, 0x07c
    uint32_t queue_desc_low;        // 0x080
    uint32_t queue_desc_high;       // 0x084
    uint32_t reserved_6[2];         // 0x088, 0x08c
    uint32_t queue_avail_low;       // 0x090
    uint32_t queue_avail_high;      // 0x094
    uint32_t reserved_7[2];         // 0x098, 0x09c
    uint32_t queue_used_low;        // 0x0A0
    uint32_t queue_used_high;       // 0x0A4
    uint32_t reserved_8[22];        // 0x0A8 ~ 0x0FC
    uint8_t config_space[];         // 0x100
};

#define VIRTIO_RNG_BASE 0x10001000
#define VIRTIO_DEVICE_ID_RNG 4
volatile struct VirtIOMMIOLayout *virtio_rng =
    (volatile struct VirtIOMMIOLayout *)VIRTIO_RNG_BASE;

uint8_t random_data[RANDOM_SIZE] __attribute__((aligned(4096)));
VirtQueueDesc desc[DESC_SIZE] __attribute__((aligned(4096)));
uint8_t avail_base[AVAIL_RING_SIZE] __attribute__((aligned(4096)));
VirtQueueAvail *avail = (VirtQueueAvail *)avail_base;
uint8_t used_base[USED_RING_SIZE] __attribute__((aligned(4096)));
VirtQueueUsed *used = (VirtQueueUsed *)used_base;

int main(void) {
    TEST_START(__func__);

    if (virtio_rng->device_id != VIRTIO_DEVICE_ID_RNG) {
        Log(ERROR, "Expected VirtIO RNG device ID, got %u\n", virtio_rng->device_id);
        fail();
    }

    virtio_rng->status = ACKNOWLEDGE;
    virtio_rng->status = ACKNOWLEDGE | DRIVER;

    virtio_rng->device_features_sel = 0;
    uint64_t host_features = virtio_rng->device_features;
    virtio_rng->device_features_sel = 1;
    host_features |= ((uint64_t)virtio_rng->device_features) << 32;

    virtio_rng->driver_features_sel = 0;
    virtio_rng->driver_features = (uint32_t)host_features;
    virtio_rng->driver_features_sel = 1;
    virtio_rng->driver_features = (uint32_t)(host_features >> 32);
    virtio_rng->status = ACKNOWLEDGE | DRIVER | FEATURES_OK;
    if (!(virtio_rng->status & FEATURES_OK)) {
        Log(ERROR, "Feature negotiation failed\n");
        fail();
    }

    virtio_rng->queue_sel = 0;
    if (virtio_rng->queue_num_max == 0) {
        Log(ERROR, "No RNG queue\n");
        fail();
    }
    virtio_rng->queue_num = QUEUE_SIZE;
    virtio_rng->queue_desc_low = (uint64_t)desc & 0xffffffff;
    virtio_rng->queue_desc_high = ((uint64_t)desc >> 32) & 0xffffffff;
    virtio_rng->queue_avail_low = (uint64_t)avail & 0xffffffff;
    virtio_rng->queue_avail_high = ((uint64_t)avail >> 32) & 0xffffffff;
    virtio_rng->queue_used_low = (uint64_t)used & 0xffffffff;
    virtio_rng->queue_used_high = ((uint64_t)used >> 32) & 0xffffffff;
    virtio_rng->queue_ready = 1;

    avail->flags = AvailFlagDefault;
    avail->idx = 0;
    used->flags = UsedFlagDefault;
    used->idx = 0;

    for (int i = 0; i < RANDOM_SIZE; i++) {
        random_data[i] = 0;
    }

    VirtQueueDesc *request = &desc[0];
    request->paddr = (uint64_t)random_data;
    request->len = RANDOM_SIZE;
    request->flags = VIRTQ_DESC_F_WRITE;
    request->next = 0;

    avail->ring[0] = 0;
    avail->idx = 1;
    virtio_rng->queue_notify = 0;

    for (volatile uint32_t spin = 0;
         !(virtio_rng->interrupt_status & VIRTIO_MMIO_INT_VRING) &&
         spin < 100000000;
         spin++) {
    }

    if (!(virtio_rng->interrupt_status & VIRTIO_MMIO_INT_VRING)) {
        Log(ERROR, "Missing used-ring interrupt\n");
        fail();
    }
    virtio_rng->interrupt_ack = VIRTIO_MMIO_INT_VRING;

    if (used->idx != 1 || used->ring[0].id != 0 ||
        used->ring[0].len != RANDOM_SIZE) {
        Log(ERROR, "Invalid used-ring completion\n");
        fail();
    }

    Log(INFO, "RNG data: ");
    for (int i = 0; i < 5; i++) {
        Log(INFO, "%02x", random_data[i]);
    }

    uint8_t first = random_data[0];
    int all_equal = 1;
    for (int i = 1; i < RANDOM_SIZE; i++) {
        if (random_data[i] != first) {
            all_equal = 0;
            break;
        }
    }
    if (all_equal) {
        Log(ERROR, "RNG output did not vary\n");
        fail();
    }

    pass();
    return 0;
}
