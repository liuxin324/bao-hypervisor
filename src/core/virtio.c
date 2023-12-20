/**
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) Bao Project and Contributors. All rights reserved.
 */

#include <virtio.h>
#include <cpu.h>
#include <vm.h>
#include <hypercall.h>
#include <ipc.h>
#include <objpool.h>
#include <config.h>

/* VirtIO instances' maximum limit */
#define VIRTIO_INSTANCES_NUM_MAX      50

/* Default value assigned to VirtIO instances */
#define VIRTIO_INSTANCE_UNINITIALIZED -1

/*!
 * @enum
 * @brief   VirtIO hypercall events
 * @note    Used by the backend VM
 */
enum VIRTIO_HYP_EVENTS {
    VIRTIO_WRITE_OP,  // Write operation
    VIRTIO_READ_OP,   // Read operation
    VIRTIO_ASK_OP,    // Ask operation (used to retrieve the next request)
    VIRTIO_NOTIFY_OP, // Notification operation (used for buffer or configuration change)
};

/*!
 * @enum
 * @brief   VirtIO cpu_msg events
 */
enum VIRTIO_CPU_MSG_EVENTS {
    VIRTIO_WRITE_NOTIFY,        // Write notification
    VIRTIO_READ_NOTIFY,         // Read notification
    VIRTIO_INJECT_INTERRUPT,    // Interrupt injection operation
    VIRTIO_NOTIFY_BACKEND_POOL, // Pooling notification
};

/*!
 * @enum
 * @brief   VirtIO direction
 */
enum VIRTIO_DIRECTION {
    VIRTIO_FRONTEND_TO_BACKEND, // Frontend to backend direction
    VIRTIO_BACKEND_TO_FRONTEND, // Backend to frontend direction
};

/*!
 * @struct  virtio_instance
 * @brief   Holds the static information regarding a VirtIO device and driver pair (VirtIO instance)
 */
struct virtio_instance {
    cpuid_t backend_cpu_id;   // CPU ID assigned to the VirtIO backend
    vmid_t backend_vm_id;     // VM ID associated with the VirtIO backend
    cpuid_t frontend_cpu_id;  // CPU ID assigned to the VirtIO frontend
    vmid_t frontend_vm_id;    // VM ID associated with the VirtIO frontend
    irqid_t virtio_interrupt; // Backend VM interrupt
    irqid_t device_interrupt; // Device interrupt
    unsigned int priority; // Driver priority for backend scheduling (Higher number indicates lower
                           // priority)
    unsigned int device_type; // Identifies the actual physical device type
    bool pooling;             // Define the backend execution mode: polling or interrupt-based
};

/*!
 * @struct  virtio_access
 * @brief   Contains the specific parameters of a VirtIO device access
 */
struct virtio_access {
    node_t node;                // Node of the list
    unsigned long reg_off;      // Offset of the accessed MMIO Register
    vaddr_t addr;               // Address of the accessed MMIO Register
    unsigned long access_width; // Access width
    unsigned long op;           // Operation
    unsigned long value;        // Value to write or read
    unsigned long reg;          // CPU register for storing the MMIO register valu
    unsigned int priority; // Driver priority for backend scheduling (Higher number indicates lower
                           // priority)
    cpuid_t frontend_cpu_id; // CPU ID assigned to the VirtIO frontend
    bool handled;            // Indicate whether the request was handled by the backend VM
};

/*!
 * @struct  virtio
 * @brief   Comprises all the information about the VirtIO infrastructure
 */
struct virtio {
    node_t node;        // Node of the list
    uint64_t virtio_id; // Unique VirtIO ID linking each frontend driver to the backend device
    enum VIRTIO_DIRECTION direction;  // Direction of the request
    struct list frontend_access_list; // Frontend access list
    struct list backend_access_list;  // Backend access list
    struct virtio_instance instance;  // Static information about the VirtIO instance
};

/*!
 * @struct  virtio_list
 * @brief   Holds all VirtIO information
 */
struct list virtio_list;

OBJPOOL_ALLOC(virtio_frontend_access_pool, struct virtio_access, sizeof(struct virtio_access));
OBJPOOL_ALLOC(virtio_backend_access_pool, struct virtio_access, sizeof(struct virtio_access));
OBJPOOL_ALLOC(virtio_pool, struct virtio, sizeof(struct virtio));

/* functions prototypes */
static void virtio_handler(uint32_t, uint64_t);
static int virtio_prio_node_cmp(node_t* _n1, node_t* _n2);

/* create the handler for the cpu_msg */
CPU_MSG_HANDLER(virtio_handler, VIRTIO_CPUMSG_ID);

void virtio_init()
{
    int i, vm_id, frontend_id = 0, backend_id = 0;
    int backend_devices[VIRTIO_INSTANCES_NUM_MAX];

    objpool_init(&virtio_pool);
    objpool_init(&virtio_frontend_access_pool);
    objpool_init(&virtio_backend_access_pool);
    list_init(&virtio_list);

    for (i = 0; i < VIRTIO_INSTANCES_NUM_MAX; i++) {
        backend_devices[i] = VIRTIO_INSTANCE_UNINITIALIZED;
    }

    for (vm_id = 0; vm_id < config.vmlist_size; vm_id++) {
        struct vm_config* vm_config = &config.vmlist[vm_id];
        for (i = 0; i < vm_config->platform.virtiodevices_num; i++) {
            struct virtio_device* dev = &vm_config->platform.virtiodevices[i];
            if (dev->is_back_end) {
                struct virtio* node = objpool_alloc(&virtio_pool);
                node->virtio_id = dev->virtio_id;
                list_push(&virtio_list, (node_t*)node);

                if (backend_devices[dev->virtio_id] != VIRTIO_INSTANCE_UNINITIALIZED) {
                    list_foreach (virtio_list, struct virtio, virtio_device) {
                        objpool_free(&virtio_pool, (struct virtio*)list_pop(&virtio_list));
                    }
                    ERROR("Failed to link backend to the device, more than one back-end was "
                          "atributed to the VirtIO instance %d",
                        dev->virtio_id);
                } else {
                    dev->backend_vm_id = vm_id;
                    backend_id++;
                    backend_devices[dev->virtio_id] = vm_id;
                }
            } else {
                dev->frontend_vm_id = vm_id;
                frontend_id++;
            }
        }
    }

    if (backend_id != frontend_id) {
        ERROR("There is no 1-to-1 mapping between a VirtIO backend and VirtIO frontend");
    }

    for (vm_id = 0; vm_id < config.vmlist_size; vm_id++) {
        struct vm_config* vm_config = &config.vmlist[vm_id];
        for (i = 0; i < vm_config->platform.virtiodevices_num; i++) {
            struct virtio_device* dev = &vm_config->platform.virtiodevices[i];
            list_foreach (virtio_list, struct virtio, virtio_device) {
                if (dev->virtio_id == virtio_device->virtio_id) {
                    if (dev->is_back_end) {
                        virtio_device->instance.backend_vm_id = dev->backend_vm_id;
                        virtio_device->instance.device_type = dev->device_type;
                        virtio_device->instance.virtio_interrupt =
                            vm_config->platform.virtio_interrupt;
                        virtio_device->instance.pooling = vm_config->platform.virtio_pooling;
                    } else {
                        virtio_device->instance.frontend_vm_id = dev->frontend_vm_id;
                        virtio_device->instance.priority = dev->priority;
                        virtio_device->instance.device_interrupt = dev->device_interrupt;
                    }
                }
            }
        }
    }
}

void virtio_assign_cpus(struct vm* vm)
{
    for (int i = 0; i < vm->virtiodevices_num; i++) {
        list_foreach (virtio_list, struct virtio, virtio_device) {
            if (vm->virtiodevices[i].virtio_id == virtio_device->virtio_id) {
                if (vm->virtiodevices[i].backend_vm_id == cpu()->vcpu->vm->id) {
                    virtio_device->instance.backend_cpu_id = cpu()->id;
                } else if (vm->virtiodevices[i].frontend_vm_id == cpu()->vcpu->vm->id) {
                    virtio_device->instance.frontend_cpu_id = cpu()->id;
                }
            }
        }
    }
}

/*!
 * @fn                  virtio_hypercall_w_r_operation
 * @brief               Performs the write or read operation by updating the value
 * @param virtio_id     Contains the virtio id
 * @param reg_off       Contains the MMIO register offset
 * @param value         Contains the register value
 * @return              true if the operation was successful, false otherwise
 */
static bool virtio_hypercall_w_r_operation(unsigned long virtio_id, unsigned long reg_off,
    unsigned long value)
{
    list_foreach (virtio_list, struct virtio, virtio_device) {
        if (virtio_device->virtio_id == virtio_id) {
            struct virtio_access* node =
                (struct virtio_access*)list_pop(&virtio_device->backend_access_list);

            if (node->reg_off != reg_off) {
                break;
            }

            node->value = value;

            struct virtio_access* frontend_node = objpool_alloc(&virtio_frontend_access_pool);
            *frontend_node = *node;
            list_push(&virtio_device->frontend_access_list, (node_t*)frontend_node);
            objpool_free(&virtio_backend_access_pool, node);
            return true;
        }
    }
    return false;
}

/*!
 * @fn              virtio_cpu_msg_handler
 * @brief           Handles the cpu_msg comming from the backend
 * @param event     Contains the message event
 * @param data      Contains the virtio id
 * @return          void
 */
static void virtio_cpu_msg_handler(uint32_t event, uint64_t data)
{
    list_foreach (virtio_list, struct virtio, virtio_device) {
        if (virtio_device->virtio_id == data) {
            struct virtio_access* node =
                (struct virtio_access*)list_pop(&virtio_device->frontend_access_list);

            switch (event) {
                case VIRTIO_READ_NOTIFY:
                    vcpu_writereg(cpu()->vcpu, node->reg, node->value);
                    break;
            }

            objpool_free(&virtio_frontend_access_pool, node);
            cpu()->vcpu->active = true;
            break;
        }
    }
}

/*!
 * @fn                  virtio_cpu_send_msg
 * @brief               Dispatches a message from the backend CPU to the frontend CPU
 * @param virtio_id     Contains the virtio id
 * @param op            Contains the operation type
 * @return              void
 */
static void virtio_cpu_send_msg(unsigned long virtio_id, unsigned long op)
{
    struct cpu_msg msg = { VIRTIO_CPUMSG_ID, VIRTIO_WRITE_NOTIFY, (uint64_t)virtio_id };
    cpuid_t target_cpu = 0;

    if (op == VIRTIO_READ_OP) {
        msg.event = VIRTIO_READ_NOTIFY;
    } else if (op == VIRTIO_NOTIFY_OP) {
        msg.event = VIRTIO_INJECT_INTERRUPT;
    }

    list_foreach (virtio_list, struct virtio, virtio_device) {
        if (virtio_device->virtio_id == virtio_id) {
            virtio_device->direction = VIRTIO_BACKEND_TO_FRONTEND;

            if (op == VIRTIO_READ_OP || op == VIRTIO_WRITE_OP) {
                struct virtio_access* node =
                    (struct virtio_access*)list_peek(&virtio_device->frontend_access_list);
                if (node == NULL) {
                    ERROR("Failed to get the element from the list");
                }
                target_cpu = node->frontend_cpu_id;
            } else {
                target_cpu = virtio_device->instance.frontend_cpu_id;
            }

            cpu_send_msg(target_cpu, &msg);
            break;
        }
    }
}

/*!
 * @fn              virtio_inject_interrupt
 * @brief           Injects an interrupt into the vCPU running the frontend or backend VM
 * @param data      Contains the virtio id
 * @return          void
 */
static void virtio_inject_interrupt(uint64_t data)
{
    irqid_t irq_id = 0;

    list_foreach (virtio_list, struct virtio, virtio_device) {
        if (virtio_device->virtio_id == data) {
            if (virtio_device->direction == VIRTIO_FRONTEND_TO_BACKEND) {
                irq_id = virtio_device->instance.virtio_interrupt;
            } else {
                irq_id = virtio_device->instance.device_interrupt;
            }
            break;
        }
    }

    if (irq_id) {
        vcpu_inject_irq(cpu()->vcpu, irq_id);
    } else {
        ERROR("Failed to inject interrupt");
    }
}

unsigned long virtio_hypercall(unsigned long arg0, unsigned long arg1, unsigned long arg2)
{
    unsigned long ret = -HC_E_SUCCESS;                // return value
    unsigned long virtio_id = cpu()->vcpu->regs.x[2]; // virtio id
    unsigned long reg_off = cpu()->vcpu->regs.x[3];   // MMIO register offset
    // unsigned long addr = cpu()->vcpu->regs.x[4];            // MMIO register address
    unsigned long op = cpu()->vcpu->regs.x[5];    // operation
    unsigned long value = cpu()->vcpu->regs.x[6]; // register value

    switch (op) {
        case VIRTIO_WRITE_OP:
        case VIRTIO_READ_OP:
            if (!virtio_hypercall_w_r_operation(virtio_id, reg_off, value)) {
                ret = -HC_E_FAILURE;
            } else {
                virtio_cpu_send_msg(virtio_id, op);
            }
            break;
        case VIRTIO_ASK_OP:
            ret = -HC_E_FAILURE;
            if (reg_off != 0 || value != 0) {
                break;
            }
            list_foreach (virtio_list, struct virtio, virtio_device) {
                if (virtio_device->virtio_id == virtio_id &&
                    cpu()->vcpu->vm->id == virtio_device->instance.backend_vm_id) {
                    list_foreach (virtio_device->backend_access_list, struct virtio_access, node) {
                        if (!node->handled) {
                            node->handled = true;
                            vcpu_writereg(cpu()->vcpu, 1, virtio_id);
                            vcpu_writereg(cpu()->vcpu, 2, node->reg_off);
                            vcpu_writereg(cpu()->vcpu, 3, node->addr);
                            vcpu_writereg(cpu()->vcpu, 4, node->op);
                            vcpu_writereg(cpu()->vcpu, 5, node->value);
                            vcpu_writereg(cpu()->vcpu, 6, node->access_width);
                            return HC_E_SUCCESS;
                        }
                    }
                    ret = -HC_E_FAILURE;
                    break;
                }
            }
            break;
        case VIRTIO_NOTIFY_OP:
            virtio_cpu_send_msg(virtio_id, op);
            break;
        default:
            ret = -HC_E_INVAL_ARGS;
            break;
    }

    return ret;
}

bool virtio_mmio_emul_handler(struct emul_access* acc)
{
    struct vm* vm = cpu()->vcpu->vm;
    struct virtio_device virtio_dev;
    volatile int i, j;

    for (i = 0; i < vm->virtiodevices_num; i++) {
        virtio_dev = vm->virtiodevices[i];
        if (acc->addr >= virtio_dev.va && acc->addr <= virtio_dev.va + virtio_dev.size) {
            break;
        }
    }

    if (i == vm->virtiodevices_num) {
        return false;
    }

    list_foreach (virtio_list, struct virtio, virtio_device) {
        if (virtio_device->virtio_id == virtio_dev.virtio_id) {
            struct virtio_access* node = objpool_alloc(&virtio_backend_access_pool);
            struct cpu_msg msg = { VIRTIO_CPUMSG_ID, VIRTIO_INJECT_INTERRUPT, virtio_dev.virtio_id };
            node->reg_off = acc->addr - virtio_dev.va;
            node->addr = acc->addr;
            node->reg = acc->reg;
            node->access_width = acc->width;
            node->priority = virtio_device->instance.priority;
            node->frontend_cpu_id = cpu()->id;
            node->handled = false;

            if (acc->write) {
                int value = vcpu_readreg(cpu()->vcpu, acc->reg);
                node->op = VIRTIO_WRITE_OP;
                node->value = value;
            } else {
                node->op = VIRTIO_READ_OP;
                node->value = 0;
            }
            for (j = 0; j < config.vmlist[virtio_dev.backend_vm_id].platform.virtiodevices_num;
                 j++) {
                if (config.vmlist[virtio_dev.backend_vm_id].platform.virtiodevices[j].virtio_id ==
                    virtio_dev.virtio_id) {
                    if (config.vmlist[virtio_dev.backend_vm_id].platform.virtiodevices[j].pooling) {
                        msg.event = VIRTIO_NOTIFY_BACKEND_POOL;
                    }
                    virtio_device->direction = VIRTIO_FRONTEND_TO_BACKEND;
                    list_insert_ordered(&virtio_device->backend_access_list, (node_t*)node,
                        virtio_prio_node_cmp);
                    cpu_send_msg(virtio_device->instance.backend_cpu_id, &msg);
                    cpu()->vcpu->regs.elr_el2 += 4;
                    cpu()->vcpu->active = false;
                    cpu_idle();
                    return true;
                }
            }
            break;
        }
    }
    return false;
}

/*!
 * @fn              virtio_handler
 * @brief           Manages incoming cpu_msg from the frontend or backend
 * @param event     Contains the message event
 * @param data      Contains the virtio id
 * @return          void
 */
static void virtio_handler(uint32_t event, uint64_t data)
{
    switch (event) {
        case VIRTIO_WRITE_NOTIFY:
        case VIRTIO_READ_NOTIFY:
            virtio_cpu_msg_handler(event, data);
            break;
        case VIRTIO_INJECT_INTERRUPT:
            virtio_inject_interrupt(data);
            break;
    }
}

/*!
 * @fn              virtio_prio_node_cmp
 * @brief           Compares two elements by priority
 * @param _n1       Contains the first node
 * @param _n2       Contains the second node
 * @return          int
 */
static int virtio_prio_node_cmp(node_t* _n1, node_t* _n2)
{
    struct virtio_access* n1 = (struct virtio_access*)_n1;
    struct virtio_access* n2 = (struct virtio_access*)_n2;

    if (n1->priority > n2->priority) {
        return 1;
    } else if (n1->priority < n2->priority) {
        return -1;
    } else {
        return 0;
    }
}
