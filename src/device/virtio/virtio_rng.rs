use std::{
    slice,
    sync::{
        Arc,
        atomic::{AtomicU8, Ordering},
    },
};

use log::{error, info};
use rand::RngCore;
use tokio::sync::{mpsc, watch};

use crate::{
    device::virtio::{
        config::{VIRTQUEUE_MAX_SIZE, VirtIOFeatureSet, virtio_reserved_feature},
        virtio_device::VirtIODeviceTrait,
        virtio_mmio::{VIRTIO_DEVICE_ID_RNG, VirtIODeviceStatus},
        virtio_queue::{VirtQueue, VirtQueueCompletion, VirtQueueDesc},
    },
    task_spawner::{DeviceTask, TaskSpawner},
};

const VIRTIO_RNG_TASK_CAPACITY: usize = VIRTQUEUE_MAX_SIZE as usize;

#[derive(Clone, Copy)]
/// A guest-writable segment retained for the asynchronous entropy worker.
struct RngRamBuffer {
    addr: usize,
    len: usize,
}

struct VirtIORngRequest {
    buffers: Vec<RngRamBuffer>,
    used_len: u32,
    completion: VirtQueueCompletion,
    isr: Arc<AtomicU8>,
}

enum VirtIORngAsyncTask {
    Fill(VirtIORngRequest),
    /// Acknowledge after all requests submitted before this marker have completed.
    DrainBarrier(std::sync::mpsc::SyncSender<()>),
}

/// Fills guest-owned entropy buffers on the device task thread.
pub(super) struct VirtIORngAsyncWorker {
    receiver: mpsc::Receiver<VirtIORngAsyncTask>,
}

impl DeviceTask for VirtIORngAsyncWorker {
    async fn run(mut self, mut cancel: watch::Receiver<bool>) {
        loop {
            let task = tokio::select! {
                _ = cancel.changed() => return,
                task = self.receiver.recv() => match task {
                    Some(task) => task,
                    None => return,
                },
            };

            self.run_task(task);
            if *cancel.borrow() {
                return;
            }
        }
    }
}

impl VirtIORngAsyncWorker {
    fn new(receiver: mpsc::Receiver<VirtIORngAsyncTask>) -> Self {
        Self { receiver }
    }

    fn run_task(&mut self, task: VirtIORngAsyncTask) {
        // Synchronize guest writes published before QueueNotify with this worker.
        std::sync::atomic::fence(Ordering::Acquire);

        match task {
            VirtIORngAsyncTask::Fill(request) => {
                for buffer in &request.buffers {
                    let data =
                        unsafe { slice::from_raw_parts_mut(buffer.addr as *mut u8, buffer.len) };
                    rand::rng().fill_bytes(data);
                }

                // The used index is published with Release ordering. The ISR is
                // then published with Release as well so the guest cannot
                // observe an interrupt before the random bytes and used entry.
                request.completion.complete(request.used_len);
                request.isr.fetch_or(1, Ordering::Release);
            }
            VirtIORngAsyncTask::DrainBarrier(ack) => {
                // This single-threaded worker has completed every earlier request.
                let _ = ack.send(());
            }
        }
    }

    #[cfg(test)]
    fn process_one(&mut self) -> bool {
        match self.receiver.try_recv() {
            Ok(task) => {
                self.run_task(task);
                true
            }
            Err(mpsc::error::TryRecvError::Empty | mpsc::error::TryRecvError::Disconnected) => {
                false
            }
        }
    }
}

pub(crate) struct VirtIORngDevice {
    status: u8,
    isr: Arc<AtomicU8>,
    host_feature: VirtIOFeatureSet,
    guest_feature: VirtIOFeatureSet,
    generation: u32,
    ram_base_raw: usize,
    sender: mpsc::Sender<VirtIORngAsyncTask>,
    queue: VirtQueue,
}

impl VirtIORngDevice {
    fn new(ram_base_raw: *mut u8, sender: mpsc::Sender<VirtIORngAsyncTask>) -> Self {
        info!("build virtio rng device.");

        Self {
            status: 0,
            isr: Arc::new(AtomicU8::new(0)),
            host_feature: virtio_reserved_feature::VERSION_1,
            guest_feature: 0,
            generation: 0,
            ram_base_raw: ram_base_raw as usize,
            sender,
            queue: VirtQueue::new(ram_base_raw, 0), // Configured by the driver.
        }
    }
}

impl VirtIODeviceTrait for VirtIORngDevice {
    fn get_device_id(&self) -> u16 {
        VIRTIO_DEVICE_ID_RNG
    }

    fn status(&mut self) -> &mut u8 {
        &mut self.status
    }

    fn get_generation(&self) -> u32 {
        self.generation
    }

    fn reset(&mut self) {
        info!("reset virtio rng device.");

        // Reset must not leave a task that can still write guest RAM after the
        // queue has been torn down. The FIFO barrier drains all earlier work.
        let (drain_tx, drain_rx) = std::sync::mpsc::sync_channel(0);
        if self
            .sender
            .blocking_send(VirtIORngAsyncTask::DrainBarrier(drain_tx))
            .is_ok()
        {
            let _ = drain_rx.recv();
        }

        self.status = 0;
        self.guest_feature = 0;
        self.queue.reset();
        self.isr.store(0, Ordering::Release);
    }

    fn isr(&self) -> &AtomicU8 {
        &self.isr
    }

    fn irq_level(&mut self) -> bool {
        self.isr.load(Ordering::Acquire) != 0 && self.queue.interrupts_enabled()
    }

    fn get_host_feature(&self) -> VirtIOFeatureSet {
        self.host_feature
    }

    fn set_feature(&mut self, feature: VirtIOFeatureSet) {
        info!("set virtio rng feature.");
        if feature & virtio_reserved_feature::VERSION_1 == 0
            || self.host_feature & feature != feature
        {
            self.status &= !VirtIODeviceStatus::FEATURES_OK.bits();
        } else {
            self.guest_feature = feature;
        }
    }

    fn set_queue_num(&mut self, num: u32) {
        self.queue.set_queue_num(num);
    }

    fn queue_select(&self, _idx: u32) {
        // VirtIO RNG has exactly one queue.
    }

    fn set_desc(&mut self, addr: u64) {
        self.queue.set_desc(addr);
    }

    fn set_avail(&mut self, addr: u64) {
        self.queue.set_avail(addr);
    }

    fn set_used(&mut self, addr: u64) {
        self.queue.set_used(addr);
    }

    fn manage_one_request(&mut self) -> bool {
        let Ok(permit) = self.sender.try_reserve() else {
            return false;
        };

        let mut buffers = Vec::new();
        let mut used_len = 0u32;
        let mut length_overflow = false;
        let ram_base_raw = self.ram_base_raw;
        let Some(completion) = self.queue.take_one_request(|desc: &VirtQueueDesc, _idx| {
            if !desc.is_device_writable() {
                error!("virtio-rng request contains a read-only descriptor");
                return;
            }

            let Some(new_len) = used_len.checked_add(desc.len) else {
                length_overflow = true;
                return;
            };
            used_len = new_len;
            buffers.push(RngRamBuffer {
                addr: desc.get_request_package::<u8>(ram_base_raw) as usize,
                len: desc.len as usize,
            });
        }) else {
            return false;
        };

        if length_overflow {
            error!("virtio-rng request length exceeds the used-ring limit");
            buffers.clear();
            used_len = 0;
        }

        permit.send(VirtIORngAsyncTask::Fill(VirtIORngRequest {
            buffers,
            used_len,
            completion,
            isr: self.isr.clone(),
        }));
        true
    }

    fn notify(&mut self, _idx: u32) {
        while self.manage_one_request() {}
    }

    fn queue_ready(&self) -> bool {
        self.queue.ready()
    }

    fn get_num_of_queue(&self) -> u32 {
        1
    }

    fn read_config(&mut self, _offset: u64, _len: u32) -> u64 {
        0
    }

    fn write_config(&mut self, _offset: u64, _len: u32, _data: u64) {}
}

pub struct VirtIORngDeviceBuilder {
    device: VirtIORngDevice,
    receiver: mpsc::Receiver<VirtIORngAsyncTask>,
}

impl VirtIORngDeviceBuilder {
    pub fn new(ram_base_raw: *mut u8) -> Self {
        let (sender, receiver) = mpsc::channel(VIRTIO_RNG_TASK_CAPACITY);
        Self {
            device: VirtIORngDevice::new(ram_base_raw, sender),
            receiver,
        }
    }

    pub fn get_and_spawner_task(self, task_spawner: &mut TaskSpawner) -> VirtIORngDevice {
        let Self { device, receiver } = self;
        task_spawner.register(VirtIORngAsyncWorker::new(receiver));
        device
    }

    #[cfg(test)]
    fn get_with_worker(self) -> (VirtIORngDevice, VirtIORngAsyncWorker) {
        let Self { device, receiver } = self;
        (device, VirtIORngAsyncWorker::new(receiver))
    }
}

#[cfg(test)]
mod tests {
    use std::slice;

    use super::*;
    use crate::{
        device::virtio::virtio_queue::{
            VirtQueueAvail, VirtQueueAvailFlag, VirtQueueDescFlag, VirtQueueUsed, VirtQueueUsedFlag,
        },
        ram::Ram,
        ram_config,
    };

    const QUEUE_NUM: usize = 8;
    const DESC_BASE: u64 = 0x8000_2000;
    const AVAIL_BASE: u64 = 0x8000_3000;
    const USED_BASE: u64 = 0x8000_4000;
    const BUFFER0_BASE: u64 = 0x8000_5000;
    const BUFFER1_BASE: u64 = 0x8000_5100;

    fn configured_device() -> (Ram, VirtIORngDevice, VirtIORngAsyncWorker) {
        let mut ram = Ram::new();
        let ram_base = &mut ram[0] as *mut u8;
        let (mut device, worker) = VirtIORngDeviceBuilder::new(ram_base).get_with_worker();
        device.set_queue_num(QUEUE_NUM as u32);
        device.set_desc(DESC_BASE);
        device.set_avail(AVAIL_BASE);
        device.set_used(USED_BASE);

        {
            let avail = unsafe {
                (&mut ram[(AVAIL_BASE - ram_config::BASE_ADDR) as usize] as *mut u8
                    as *mut VirtQueueAvail)
                    .as_mut()
                    .unwrap()
            };
            avail.init(VirtQueueAvailFlag::Default);
        }
        {
            let used = unsafe {
                (&mut ram[(USED_BASE - ram_config::BASE_ADDR) as usize] as *mut u8
                    as *mut VirtQueueUsed)
                    .as_mut()
                    .unwrap()
            };
            used.init(VirtQueueUsedFlag::Default);
        }

        (ram, device, worker)
    }

    fn guest_slice(ram: &Ram, address: u64, len: usize) -> &[u8] {
        let offset = (address - ram_config::BASE_ADDR) as usize;
        let base = &ram[offset] as *const u8;
        unsafe { slice::from_raw_parts(base, len) }
    }

    #[test]
    fn device_exposes_virtio_rng_identity_and_version() {
        let mut ram = Ram::new();
        let device = VirtIORngDeviceBuilder::new(&mut ram[0] as *mut u8)
            .get_with_worker()
            .0;

        assert_eq!(device.get_device_id(), 4);
        assert_eq!(
            device.get_host_feature(),
            virtio_reserved_feature::VERSION_1
        );
        assert_eq!(device.get_num_of_queue(), 1);
    }

    #[test]
    fn worker_fills_every_writable_descriptor_and_publishes_interrupt() {
        let (mut ram, mut device, mut worker) = configured_device();
        let avail = unsafe {
            &mut *(&mut ram[(AVAIL_BASE - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut VirtQueueAvail)
        };
        let avail_ring =
            VirtQueueAvail::mut_ring(avail as *mut VirtQueueAvail as u64, QUEUE_NUM as u32);
        let descriptors = unsafe {
            slice::from_raw_parts_mut(
                &mut ram[(DESC_BASE - ram_config::BASE_ADDR) as usize] as *mut u8
                    as *mut VirtQueueDesc,
                QUEUE_NUM,
            )
        };

        descriptors[0].init(
            BUFFER0_BASE,
            256,
            VirtQueueDescFlag::VIRTQ_DESC_F_WRITE | VirtQueueDescFlag::VIRTQ_DESC_F_NEXT,
            1,
        );
        descriptors[1].init(BUFFER1_BASE, 256, VirtQueueDescFlag::VIRTQ_DESC_F_WRITE, 0);
        avail_ring[0] = 0;
        avail.idx_atomic_add(1);

        assert!(device.manage_one_request());
        assert!(worker.process_one());

        let buffer0 = guest_slice(&ram, BUFFER0_BASE, 256);
        let buffer1 = guest_slice(&ram, BUFFER1_BASE, 256);
        assert!(buffer0.iter().chain(buffer1.iter()).any(|byte| *byte != 0));
        assert_eq!(device.isr().load(Ordering::Acquire), 1);
        assert!(device.irq_level());
        assert_eq!(device.queue.get_used_ring().get_index(), 1);
        let used = device.queue.get_used_ring().ring(QUEUE_NUM as u32)[0];
        assert_eq!(used.get_id(), 0);
        assert_eq!(used.get_len(), 512);
    }

    #[test]
    fn read_only_descriptor_is_not_written() {
        let (mut ram, mut device, mut worker) = configured_device();
        let avail = unsafe {
            &mut *(&mut ram[(AVAIL_BASE - ram_config::BASE_ADDR) as usize] as *mut u8
                as *mut VirtQueueAvail)
        };
        let avail_ring =
            VirtQueueAvail::mut_ring(avail as *mut VirtQueueAvail as u64, QUEUE_NUM as u32);
        let descriptors = unsafe {
            slice::from_raw_parts_mut(
                &mut ram[(DESC_BASE - ram_config::BASE_ADDR) as usize] as *mut u8
                    as *mut VirtQueueDesc,
                QUEUE_NUM,
            )
        };

        descriptors[0].init(BUFFER0_BASE, 64, VirtQueueDescFlag::empty(), 0);
        avail_ring[0] = 0;
        avail.idx_atomic_add(1);

        assert!(device.manage_one_request());
        assert!(worker.process_one());
        assert!(
            guest_slice(&ram, BUFFER0_BASE, 64)
                .iter()
                .all(|byte| *byte == 0)
        );
        assert_eq!(device.queue.get_used_ring().get_index(), 1);
        assert_eq!(
            device.queue.get_used_ring().ring(QUEUE_NUM as u32)[0].get_len(),
            0
        );
    }
}
