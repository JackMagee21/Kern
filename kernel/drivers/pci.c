#include "pci.h"
#include "../../libk/io.h"

/*
 * Legacy PCI "Configuration Mechanism #1": write a 32-bit request to
 * CONFIG_ADDRESS (bit 31 = enable, bits 23-16 = bus, bits 15-11 =
 * device, bits 10-8 = function, bits 7-2 = dword-aligned register
 * offset), then read/write the requested dword through CONFIG_DATA.
 * Ports and bit layout verified against the OSDev.org PCI wiki
 * article's documentation of the PCI Local Bus Specification's
 * Configuration Mechanism #1 -- unchanged, standard, and universally
 * supported since the original PCI spec (no ACPI/MCFG-based
 * Enhanced Configuration Access Mechanism needed for this).
 */
#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu

#define PCI_HEADER_TYPE_MULTIFUNCTION 0x80u

static uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t address = (1u << 31)
                      | ((uint32_t)bus << 16)
                      | ((uint32_t)device << 11)
                      | ((uint32_t)function << 8)
                      | (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFCu);
    return (uint16_t)(dword >> ((offset & 2u) * 8u));
}

static uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFCu);
    return (uint8_t)(dword >> ((offset & 3u) * 8u));
}

static void probe_function(uint8_t bus, uint8_t device, uint8_t function,
                            pci_device_callback_t on_device, void *ctx, uint32_t *count)
{
    uint16_t vendor_id = pci_config_read_word(bus, device, function, 0x00);
    if (vendor_id == 0xFFFFu) {
        return; /* 0xFFFF is not a real vendor ID -- the standard "nothing here" sentinel */
    }

    pci_device_t dev = {
        .bus = bus,
        .device = device,
        .function = function,
        .vendor_id = vendor_id,
        .device_id = pci_config_read_word(bus, device, function, 0x02),
        .class_code = pci_config_read_byte(bus, device, function, 0x0B),
        .subclass = pci_config_read_byte(bus, device, function, 0x0A),
        .prog_if = pci_config_read_byte(bus, device, function, 0x09),
        .header_type = pci_config_read_byte(bus, device, function, 0x0E),
    };
    on_device(&dev, ctx);
    (*count)++;
}

uint32_t pci_scan(pci_device_callback_t on_device, void *ctx)
{
    uint32_t count = 0;

    for (uint32_t bus = 0; bus < 256u; bus++) {
        for (uint32_t device = 0; device < 32u; device++) {
            uint16_t vendor_id = pci_config_read_word((uint8_t)bus, (uint8_t)device, 0, 0x00);
            if (vendor_id == 0xFFFFu) {
                continue; /* function 0 absent -- nothing else at this device to probe */
            }

            uint8_t header_type = pci_config_read_byte((uint8_t)bus, (uint8_t)device, 0, 0x0E);
            probe_function((uint8_t)bus, (uint8_t)device, 0, on_device, ctx, &count);

            if (header_type & PCI_HEADER_TYPE_MULTIFUNCTION) {
                for (uint8_t function = 1; function < 8; function++) {
                    probe_function((uint8_t)bus, (uint8_t)device, function, on_device, ctx, &count);
                }
            }
        }
    }

    return count;
}
