#ifndef KERNEL_ARCH_X86_64_REBOOT_H
#define KERNEL_ARCH_X86_64_REBOOT_H

/* Resets the CPU via the legacy 8042 keyboard controller's "pulse
   output line 0" command -- the standard non-ACPI reset mechanism
   BIOSes and OSes have relied on since long before ACPI existed, and
   still the right choice here: proper ACPI-based shutdown/reset needs
   ACPI table parsing, a CLAUDE.md non-goal pending explicit
   confirmation, which this doesn't touch. If the controller doesn't
   actually respond (rare/some emulators), falls back to an
   intentional triple fault via a deliberately invalid IDTR, which
   resets the CPU unconditionally with no hardware-specific assumption
   needed. Never returns either way. */
void reboot(void) __attribute__((noreturn));

#endif /* KERNEL_ARCH_X86_64_REBOOT_H */
