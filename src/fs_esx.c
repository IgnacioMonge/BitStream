// ============================================================================
// fs_esx.c - Storage backend: esxDOS (divMMC / divIDE)
// ============================================================================
// Filenames go in both HL (programs) and IX (dot commands): esxDOS reads the
// one that matches the caller context. IX is preserved for the C runtime.
// ============================================================================

extern uint8_t detect_esxdos(void);
extern int __LIB__ esxdos_f_unlink(void *filename) __smallc __z88dk_fastcall;

static uint8_t fs_init(void)
{
    return fs_available = detect_esxdos();
}

// Open `name` with esxDOS mode B. Returns handle or FS_BAD.
static uint8_t esx_open_mode(const char *name, uint8_t mode)
{
    (void)name; (void)mode;
    __asm
        push ix
        ld hl, 4
        add hl, sp
        ld b, (hl)          ; mode (2nd arg, pushed last)  [SP+4 after push ix]
        ld hl, 6
        add hl, sp
        ld hl, (hl)         ; name (1st arg)
        push bc
        push hl
        xor a
        rst 0x08
        defb 0x89           ; M_GETSETDRV: A = default drive
        pop hl
        pop bc
        jr c, esx_om_fail
        push hl
        pop ix
        rst 0x08
        defb 0x9A           ; F_OPEN
        jr c, esx_om_fail
        ld l, a
        jr esx_om_done
    esx_om_fail:
        ld l, 255
    esx_om_done:
        ld h, 0
        pop ix
    __endasm;
}

static uint8_t fs_create(const char *name) __z88dk_fastcall
{
    return esx_open_mode(name, 0x0E);       // FA_WRITE | FA_CREATE_AL (truncate)
}

static uint8_t fs_exists(const char *name) __z88dk_fastcall
{
    uint8_t h = esx_open_mode(name, 0x01);  // FA_READ
    if (h == FS_BAD) return 0;
    fs_close(h);
    return 1;
}

// Globals: esxDOS takes A/IX/BC, simpler to marshal through memory
static uint8_t esx_handle;
static const void *esx_buffer;
static uint16_t esx_length;

static uint16_t fs_write(uint8_t h, const void *buf, uint16_t len)
{
    esx_handle = h;
    esx_buffer = buf;
    esx_length = len;

    __asm
        push ix
        ld a, (_esx_handle)
        ld hl, (_esx_buffer)
        push hl
        pop ix
        ld bc, (_esx_length)
        rst 0x08
        defb 0x9E           ; F_WRITE
        jr c, esx_write_fail
        ld h, b
        ld l, c
        jr esx_write_done
    esx_write_fail:
        ld hl, 0
    esx_write_done:
        pop ix
    __endasm;
}

static void fs_close(uint8_t h) __z88dk_fastcall
{
    (void)h;
    __asm
        ld a, l
        push af
        rst 0x08
        defb 0x9C           ; F_SYNC
        pop af
        rst 0x08
        defb 0x9B           ; F_CLOSE
    __endasm;
}

static void fs_remove(const char *name) __z88dk_fastcall
{
    esxdos_f_unlink((void *)name);
}

static uint8_t fs_commit(const char *name) __z88dk_fastcall
{
    (void)name;
    return 1;                               // esxDOS writes are durable
}
