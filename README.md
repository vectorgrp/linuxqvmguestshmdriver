# Linux/Android Guest Shared Memory Driver for QNX Hypervisor

A Linux kernel module that enables guest VMs to access shared memory regions via the QNX vdev shmem to exchange data with applications in other VMs or the host.

## Features

- **vdev Access**: Binds to vdev shmem via PCI or MMIO.
- **Flexible region modes**: Supports multi-peer (with/without notifications) and two-peer shared memory regions
- **User-space accessible**: Shared memory regions are mmap-able into user-space applications
- **Interrupt handling**: Supports shared IRQ notifications for peer synchronization

## vdev shmem Access

| Mode | Discovery |
|-----------|-----------|
| **PCI** | Vendor/Device ID lookup |
| **MMIO** | Device Tree / ACPI |

To use the MMIO mode, the parameter mmio_addr=0xXXXXXXXX" must be used when loading the kernel module. The address must match the configuration of the vdev in the qvm config file. Example qvmconfig entry for the vdem shmem

```C
vdev vdev-shmem.so loc 0xC1000000 intr gic:17
```

### MMIO x68
For x68 platform, the MMIO is using ACPI. For this the follwoing entry must be added to the ACPI table:

```C
DefinitionBlock ("qvm_guest_shm.ssdt.aml", "SSDT", 2, "QNXQVM", "qvm_dsdt", 0x00000001)
{
    Scope (\_SB)
    {
        Device (SHM0)
        {
            Name (_HID, "SHM0001")  // _HID: Hardware ID
            Name (_UID, Zero)  // _UID: Unique ID
            Name (_DDN, "QVM Guest Shared Memory Device")  // _DDN: DOS Device Name
            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }

            Name (_CRS, ResourceTemplate ()  // _CRS: Current Resource Settings
            {
                Memory32Fixed (ReadWrite,
                    0xC1000000,         // Address Base
                    0x00001000,         // Address Length
                    )
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, ,, )
                {
                    17,
                }
            })
        }
    }
}
```

The address (e.g. 0xC1000000) and the Interrupt id (e.g. 17) must match the configuration of the vdev shmem in the qvmconfig file.

## Usage

### Loading the kernel module
Load the kernel module for MMIO mode:

```C
"insmod ./qvm_shm.ko mmio_addr=0xC1000000"
```

### Runtime access
The kernel module (`qvm_shm.ko`) exports a management character device (`/dev/ves_qvm_shm`) for creating and managing shared memory regions via ioctl.

Applications use ioctl commands on the management device to create or connect to named shared memory regions, then mmap the returned file descriptor to access shared memory.

### Supported ioctl Commands

#### Management Device ioctls

Called on `/dev/ves_qvm_shm` (the management device file descriptor).

#### `VES_QVM_SHM_CREATE_NOTIFY_MANY`
- **Type**: `_IOWR` (read/write)
- **Parameter**: `struct ves_qvm_shm_create_params`
- **Description**: Create or connect to a multi-peer shared memory region with IRQ notifications enabled.
- **Returns**: File descriptor on success; negative errno on error.

#### `VES_QVM_SHM_CREATE_NOTIFY_TWO`
- **Type**: `_IOWR` (read/write)
- **Parameter**: `struct ves_qvm_shm_create_params`
- **Description**: Create or connect to a two-peer shared memory region with IRQ notifications enabled. Only two clients can connect (client IDs 0 and 1).
- **Returns**: File descriptor on success; negative errno on error.

#### `VES_QVM_SHM_CREATE_NO_NOTIFY_MANY`
- **Type**: `_IOWR` (read/write)
- **Parameter**: `struct ves_qvm_shm_create_params`
- **Description**: Create or connect to a multi-peer shared memory region without IRQ notifications. No `read()` events; `write()` is not supported.
- **Returns**: File descriptor on success; negative errno on error.

#### SHM File Descriptor ioctls

Called on the returned file descriptor from a CREATE ioctl.

#### `VES_QVM_SHM_GET_CLIENT_ID`
- **Type**: `_IOR` (read-only)
- **Parameter**: `struct ves_qvm_shm_get_client_id_params`
- **Description**: Query this client's assigned ID within the region (0-15).
- **Returns**: 0 on success; negative errno on error.

#### Example Workflow

```c
// Open management device
int mgmt_fd = open("/dev/ves_qvm_shm", O_RDWR);

// Create a shared region with notifications
struct ves_qvm_shm_create_params params = {
    .shm_name = "my_region",
    .requested_pages = 256
};
int shm_fd = ioctl(mgmt_fd, VES_QVM_SHM_CREATE_NOTIFY_MANY, &params);

// Get this client's ID
struct ves_qvm_shm_get_client_id_params client_params = {0};
ioctl(shm_fd, VES_QVM_SHM_GET_CLIENT_ID, &client_params); 
printf("Connected as client %u\n", client_params.client_id);

// Map shared memory to user-space
void *shm = mmap(NULL, params.mapped_pages * 4096, PROT_READ | PROT_WRITE, 
                 MAP_SHARED, shm_fd, 0);

// Write data to shared memory
((char *)shm)[0] = 'A';
((char *)shm)[1] = 'B';

// Notify all other peers
struct ves_qvm_shm_write_notify notif = {
    .target = VES_QVM_SHM_NOTIFY_ALL_BUT_ME
};
write(shm_fd, &notif, sizeof(notif));

// Use poll() for non-blocking notification check
struct pollfd pfd = {
    .fd = shm_fd,
    .events = POLLIN
};
int poll_ret = poll(&pfd, 1, 5000); // 5 second timeout
if (poll_ret > 0 && (pfd.revents & POLLIN)) {
    n = read(shm_fd, &event, sizeof(event));
    if (n == sizeof(event)) {
        printf("Received notification:\n");
        printf("  Active clients: 0x%04x\n", event.active_ids);
        printf("  Disconnected clients: 0x%04x\n", event.disconnected_ids);
        printf("  Clients with user notifications: 0x%04x\n", event.user_notify_ids);
    }

// Cleanup
munmap(shm, params.mapped_pages * 4096);
close(shm_fd);
close(mgmt_fd);
```

# Build

This component builds a Linux kernel module (qvm_shm.ko) via kbuild. It is best practice to build your kernel module with the same toolchain as your kernel because of ABI compatibility and compiler behavior. 

## Required dependencies

### Tools

- GNU make (required by kbuild integration).

### Kernel build tree / headers

- You must provide a Linux kernel build tree. Currently supported kernel version: 6.1.118. Linux kernel headers are platform-specific. 
- QNX guest header for using the vdev shmem: "quest_shm.h"

## Build with make/kbuild

1. Prepare output directory

```shell
    mkdir -p build/kbuild
```

2. Build module with kbuild directly

```shell
    make -C *Path to kernel header* \
        M=$(pwd)/build/kbuild \
        src=$(pwd)/qvm-shm \
        W=1 \
        modules
```

3. Optional clean

```shell
    make -C *Path to kernel header* \
        M=$(pwd)/build/kbuild \
        src=$(pwd)/qvm-shm \
        clean
```

## Contact

Vector Informatik GmbH

`support [at] vector.com`
