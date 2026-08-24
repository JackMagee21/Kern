#ifndef KERNEL_DRIVERS_PCI_H
#define KERNEL_DRIVERS_PCI_H

#include <stdint.h>

/* One found PCI function -- a single device can expose up to 8 of
   these (header_type bit 7 set means "multi-function"), each with its
   own independent vendor/device/class identity. class_code/subclass/
   prog_if together classify what kind of device this is (e.g.
   class_code 0x01 = mass storage, subclass 0x01 = IDE) -- the standard
   PCI Class Code table, needed by any future driver that wants to find
   "the disk controller" rather than a specific vendor/device ID. */
typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
} pci_device_t;

typedef void (*pci_device_callback_t)(const pci_device_t *dev, void *ctx);

/* Brute-force scans every (bus, device, function) via the legacy
   CONFIG_ADDRESS/CONFIG_DATA I/O port mechanism (PCI Local Bus
   Specification's "Configuration Mechanism #1", ports 0xCF8/0xCFC --
   cross-checked against the OSDev.org PCI wiki article's long-stable
   documentation of this exact mechanism, the same kind of source this
   codebase already treats as authoritative for legacy hardware
   register layouts, e.g. ADR 0005's PIC/PIT ports). Calls on_device
   once per present function found (function 0's header_type decides
   whether functions 1-7 are even worth probing -- bit 7 set means
   multi-function; if clear, the device has only function 0). Returns
   the total count found. Doesn't touch any device found -- read-only
   enumeration, no driver behavior yet. */
uint32_t pci_scan(pci_device_callback_t on_device, void *ctx);

#endif /* KERNEL_DRIVERS_PCI_H */
