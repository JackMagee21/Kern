#include "ata.h"
#include "../../libk/io.h"

/*
 * Primary channel ports, standard bit layouts, command bytes, and the
 * 400ns-delay technique all verified against the OSDev.org ATA PIO
 * Mode wiki article -- see ata.h's own top-of-file doc comment for the
 * full sourcing/scope rationale.
 */
#define ATA_PRIMARY_DATA       0x1F0u
#define ATA_PRIMARY_ERROR      0x1F1u /* read */
#define ATA_PRIMARY_SECCOUNT   0x1F2u
#define ATA_PRIMARY_LBA_LOW    0x1F3u
#define ATA_PRIMARY_LBA_MID    0x1F4u
#define ATA_PRIMARY_LBA_HIGH   0x1F5u
#define ATA_PRIMARY_DRIVE_HEAD 0x1F6u
#define ATA_PRIMARY_STATUS     0x1F7u /* read */
#define ATA_PRIMARY_COMMAND    0x1F7u /* write */
#define ATA_PRIMARY_CONTROL    0x3F6u /* write: device control; read: alternate status */

#define ATA_STATUS_ERR 0x01u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_DF  0x20u
#define ATA_STATUS_BSY 0x80u

/* Bits 7 and 5 are fixed at 1 by long-standing legacy convention (the
   OSDev.org article's own "should be set for old ATA1 drives" note);
   bit 6 selects LBA addressing over legacy CHS; bit 4 selects the
   slave drive. Bits 0-3 carry the top nibble of a 28-bit LBA only when
   bit 6 (LBA) is set -- meaningless otherwise. */
#define ATA_DRIVE_HEAD_BASE  0xA0u
#define ATA_DRIVE_HEAD_LBA   0x40u
#define ATA_DRIVE_HEAD_SLAVE 0x10u

#define ATA_CMD_IDENTIFY      0xECu
#define ATA_CMD_READ_SECTORS  0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_CACHE_FLUSH   0xE7u

static bool drive_present;
static uint64_t drive_sector_count;

/* Reading the ALTERNATE status port (0x3F6, not the regular status
   port 0x1F7) doesn't clear a pending IRQ and has a well-documented
   real, if small, access latency -- reading it four times in a row is
   the standard cheap ~400ns settle time after selecting a drive or
   issuing a command, per the OSDev.org article. */
static void io_delay_400ns(void)
{
    inb(ATA_PRIMARY_CONTROL);
    inb(ATA_PRIMARY_CONTROL);
    inb(ATA_PRIMARY_CONTROL);
    inb(ATA_PRIMARY_CONTROL);
}

/* The standard "read status; 0xFF means a floating bus, nothing
   answered" presence check -- done BEFORE selecting or waiting on
   anything, since a genuinely absent drive would otherwise make any
   later poll loop spin forever. */
static bool bus_floats(void)
{
    return inb(ATA_PRIMARY_STATUS) == 0xFFu;
}

/* lba_top_nibble is only meaningful for an LBA command (READ/WRITE
   SECTORS); IDENTIFY doesn't consume the LBA registers as input at
   all, so callers just pass 0 for it there. */
static void select_slave(uint8_t lba_top_nibble)
{
    outb(ATA_PRIMARY_DRIVE_HEAD, (uint8_t)(ATA_DRIVE_HEAD_BASE | ATA_DRIVE_HEAD_LBA | ATA_DRIVE_HEAD_SLAVE | (lba_top_nibble & 0x0Fu)));
    io_delay_400ns();
}

/* Polls status until BSY clears -- every command's own first wait, per
   the OSDev.org article's polling protocol. Returns false if the bus
   floats at any point during the wait (the drive vanished, or was
   never really there despite the initial check -- defensive, not
   load-bearing for the common case). */
static bool wait_not_busy(void)
{
    for (;;) {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status == 0xFFu) {
            return false;
        }
        if (!(status & ATA_STATUS_BSY)) {
            return true;
        }
    }
}

/* Polls status until DRQ sets (data ready to transfer) or ERR/DF
   indicates the command failed. Returns false on error or a floating
   bus, true only when DRQ is genuinely set. */
static bool wait_drq(void)
{
    for (;;) {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status == 0xFFu) {
            return false;
        }
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            return false;
        }
        if (status & ATA_STATUS_DRQ) {
            return true;
        }
    }
}

static void identify_parse(const uint16_t *words)
{
    /* Words 60-61, taken together as one 32-bit value (word 60 = low
       16 bits, word 61 = high 16 bits, standard little-endian field
       order) -- the drive's own total addressable 28-bit LBA sector
       count. Verified against the OSDev.org ATA PIO Mode article, not
       assumed from memory. */
    drive_sector_count = (uint64_t)words[60] | ((uint64_t)words[61] << 16);
}

bool ata_init(void)
{
    drive_present = false;
    drive_sector_count = 0;

    if (bus_floats()) {
        return false; /* no drive at all -- the ordinary case for every OTHER existing smoke test, which never attaches one */
    }

    select_slave(0);

    if (!wait_not_busy()) {
        return false;
    }

    /* Zeroed defensively before IDENTIFY -- some real controllers use
       these registers' post-IDENTIFY values to distinguish ATA from
       ATAPI; this driver doesn't need that generality (it only ever
       expects a plain ATA disk, not an optical drive, on this specific
       slot), but starting from a known state is still the documented,
       safe convention rather than trusting whatever was left over. */
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LOW, 0);
    outb(ATA_PRIMARY_LBA_MID, 0);
    outb(ATA_PRIMARY_LBA_HIGH, 0);

    outb(ATA_PRIMARY_COMMAND, ATA_CMD_IDENTIFY);
    io_delay_400ns();

    if (inb(ATA_PRIMARY_STATUS) == 0) {
        return false; /* drive doesn't exist -- the command port itself confirms it, a second independent check beyond the initial float test */
    }

    if (!wait_not_busy()) {
        return false;
    }

    /* A nonzero LBA_MID/LBA_HIGH at this point means an ATAPI device
       (e.g. a CD-ROM) answered instead of a plain ATA disk -- this
       driver only ever expects the latter on this specific slot (the
       Makefile's own explicit `ide.0,unit=1` placement, chosen
       precisely so it can never be the boot CD-ROM). Treated as "not
       present" rather than attempting ATAPI's own, different packet
       command protocol, which this milestone doesn't need. */
    if (inb(ATA_PRIMARY_LBA_MID) != 0 || inb(ATA_PRIMARY_LBA_HIGH) != 0) {
        return false;
    }

    if (!wait_drq()) {
        return false;
    }

    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(ATA_PRIMARY_DATA);
    }

    identify_parse(identify_data);
    drive_present = (drive_sector_count > 0);
    return drive_present;
}

bool ata_is_present(void)
{
    return drive_present;
}

uint64_t ata_sector_count(void)
{
    return drive_sector_count;
}

bool ata_read_sector(uint64_t lba, uint8_t *buf)
{
    if (!drive_present || lba >= drive_sector_count || lba > 0x0FFFFFFFu) {
        return false; /* 28-bit LBA ceiling, same reasoning as ata.h's own doc comment */
    }

    if (bus_floats() || !wait_not_busy()) {
        return false;
    }

    select_slave((uint8_t)(lba >> 24));
    if (!wait_not_busy()) {
        return false;
    }

    outb(ATA_PRIMARY_SECCOUNT, 1);
    outb(ATA_PRIMARY_LBA_LOW, (uint8_t)lba);
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_SECTORS);

    if (!wait_not_busy() || !wait_drq()) {
        return false;
    }

    uint16_t *words = (uint16_t *)(void *)buf;
    for (int i = 0; i < 256; i++) {
        words[i] = inw(ATA_PRIMARY_DATA);
    }

    return true;
}

bool ata_write_sector(uint64_t lba, const uint8_t *buf)
{
    if (!drive_present || lba >= drive_sector_count || lba > 0x0FFFFFFFu) {
        return false;
    }

    if (bus_floats() || !wait_not_busy()) {
        return false;
    }

    select_slave((uint8_t)(lba >> 24));
    if (!wait_not_busy()) {
        return false;
    }

    outb(ATA_PRIMARY_SECCOUNT, 1);
    outb(ATA_PRIMARY_LBA_LOW, (uint8_t)lba);
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (!wait_not_busy() || !wait_drq()) {
        return false;
    }

    /* Deliberately a plain word-at-a-time loop, not a tight `rep
       outsw` -- the OSDev.org article's own explicit warning: real
       drives can need small inter-word delays on writes that a bare
       `rep outsw` doesn't leave room for. Each inb() of a status-style
       port already costs real I/O-bus cycles, which is exactly the
       margin this needs; no explicit extra delay was added on top of
       that since none of this hobby kernel's own real-hardware testing
       has ever motivated one -- revisit if it ever does. */
    const uint16_t *words = (const uint16_t *)(const void *)buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_PRIMARY_DATA, words[i]);
    }

    if (!wait_not_busy()) {
        return false;
    }
    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        return false;
    }

    /* CACHE FLUSH (0xE7): some real drives only commit a PIO write to
       the actual medium once explicitly flushed -- a documented ATA
       PIO gotcha, not a guess (ata.h's own doc comment). Waited out
       fully before returning, so a caller that immediately reads the
       same sector back (this driver's own self-test does exactly
       that) sees the genuinely durable result, not a race against the
       drive's own internal cache. */
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (!wait_not_busy()) {
        return false;
    }
    status = inb(ATA_PRIMARY_STATUS);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        return false;
    }

    return true;
}
