// ============================================================================
// fs_spectranext.c - Storage backend: Spectranext XFS (no esxDOS, no SD)
// ============================================================================
// docs/storage.md rules applied:
//   - Pin slot 0 (xfs://ram/) before every path operation: writes go to the
//     current slot, which the user or a boot program may have moved to a
//     network mount.
//   - New files live in the RAM layer until committed (CHMOD with the
//     commit flag); downloads are committed after a verified close.
//   - A failed download is unlinked so a retry never meets a partial file.
//   - WRITE takes the buffer in HL (IXCALL); file fds are closed with VCLOSE,
//     never with the socket CLOSE.
// ============================================================================

#define ROM_OPEN           0x3EB1   // IXCALL: HL path, DE flags, BC mode
#define ROM_UNLINK         0x3EB4   // IXCALL: HL path
#define ROM_CHMOD          0x3EC6   // IXCALL: HL path, DE mode
#define ROM_WRITE          0x3ECC   // IXCALL: A fd, HL buf, BC len -> BC
#define ROM_VCLOSE         0x3ED2   // HLCALL: A fd
#define ROM_SETMOUNTPOINT  0x3EE7   // HLCALL: A slot

#define O_RDONLY           0x0001
#define O_WRONLY           0x0002
#define O_CREAT            0x0100
#define O_TRUNC            0x0200
#define XFS_COMMIT         0x8000

static void xfs_pin_slot(void)
{
    spxn_regs.a = 0;
    spxn_rom_hlcall(ROM_SETMOUNTPOINT);
}

static uint8_t xfs_open(const char *name, uint16_t flags)
{
    xfs_pin_slot();
    spxn_regs.hl = (uint16_t)name;
    spxn_regs.de = flags;
    spxn_regs.bc = 0x01B6;                  // 0666
    if (spxn_rom_ixcall(ROM_OPEN) & ROM_CARRY) return FS_BAD;
    return spxn_regs.a;
}

static uint8_t fs_init(void)
{
    return fs_available = (spxn_rom_detect() == 1);
}

static uint8_t fs_create(const char *name) __z88dk_fastcall
{
    return xfs_open(name, O_WRONLY | O_CREAT | O_TRUNC);
}

static uint8_t fs_exists(const char *name) __z88dk_fastcall
{
    uint8_t h = xfs_open(name, O_RDONLY);
    if (h == FS_BAD) return 0;
    fs_close(h);
    return 1;
}

static uint16_t fs_write(uint8_t h, const void *buf, uint16_t len)
{
    spxn_regs.a = h;
    spxn_regs.hl = (uint16_t)buf;
    spxn_regs.bc = len;
    if (spxn_rom_ixcall(ROM_WRITE) & ROM_CARRY) return 0;
    return spxn_regs.bc;
}

static void fs_close(uint8_t h) __z88dk_fastcall
{
    spxn_regs.a = h;
    spxn_rom_hlcall(ROM_VCLOSE);
}

static void fs_remove(const char *name) __z88dk_fastcall
{
    xfs_pin_slot();
    spxn_regs.hl = (uint16_t)name;
    spxn_rom_ixcall(ROM_UNLINK);
}

static uint8_t fs_commit(const char *name) __z88dk_fastcall
{
    xfs_pin_slot();
    spxn_regs.hl = (uint16_t)name;
    spxn_regs.de = XFS_COMMIT;
    return !(spxn_rom_ixcall(ROM_CHMOD) & ROM_CARRY);
}
