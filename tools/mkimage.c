/* Build the BIOS disk images and the initial UMDFS volume on the host. */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif
#include "sha3.h"

enum {
    SECTOR = 512, FLOPPY_SECTORS = 2880, HDD_SECTORS = 32768,
    FS_LBA = 512, FS_SECTORS = 256,
    DIR_BLOCK = 1, DIR_COUNT = 32, DIR_SIZE = 64, BITMAP_BLOCK = 5,
    DATA_BLOCK = 8
};

static u8 disk[HDD_SECTORS * SECTOR];
static u8 *const fs = disk + FS_LBA * SECTOR;
static u32 entries;
static u32 next_block = DATA_BLOCK;

static void put32(u8 *dest, u32 value)
{
    for (unsigned i = 0; i < 4; ++i)
        dest[i] = (u8)(value >> (8 * i));
}

static void mark_block(u32 block)
{
    fs[BITMAP_BLOCK * SECTOR + block / 8] |= (u8)(1u << (block % 8));
}

static u32 add_entry(const char *name, u32 parent, const char *text)
{
    u32 index = entries++;
    u8 *entry = fs + DIR_BLOCK * SECTOR + index * DIR_SIZE;
    memcpy(entry, name, strlen(name));
    entry[40] = 1;
    entry[41] = text == NULL;
    put32(entry + 44, parent);
    if (text) {
        size_t length = strlen(text);
        put32(entry + 32, (u32)length);
        put32(entry + 36, next_block);
        memcpy(fs + next_block * SECTOR, text, length);
        for (size_t offset = 0; offset < length; offset += SECTOR)
            mark_block(next_block++);
    }
    return index;
}

static int random_salts(u8 salts[16])
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, salts, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE *source = fopen("/dev/urandom", "rb");
    if (!source)
        return 0;
    size_t count = fread(salts, 1, 16, source);
    fclose(source);
    return count == 16;
#endif
}

static void hex(char *out, const u8 *data, size_t length)
{
    const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        *out++ = digits[data[i] >> 4];
        *out++ = digits[data[i] & 15];
    }
    *out = 0;
}

static void credential(char out[82], const u8 salt[8], const char *password)
{
    u8 message[8 + 5], digest[32];
    memcpy(message, salt, 8);
    memcpy(message + 8, password, 5);
    sha3_256(message, sizeof(message), digest);
    hex(out, salt, 8);
    out[16] = '$';
    hex(out + 17, digest, sizeof(digest));
}

static u32 fnv1a(u32 hash, const u8 *data, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        hash = (hash ^ data[i]) * UINT32_C(16777619);
    return hash;
}

static int build_fs(void)
{
    u8 salts[16];
    char user[82], admin[82], auth[512];
    if (!random_salts(salts)) {
        fputs("Could not obtain random bytes for UMDFS credentials\n", stderr);
        return 0;
    }
    credential(user, salts, "umdos");
    credential(admin, salts + 8, "admin");
    snprintf(auth, sizeof(auth), "# UMDos /config/auth\n"
             "user=user\nuser_auth=sha3-256$%s\n"
             "administrator=administrator\nadmin_auth=sha3-256$%s\n", user, admin);

    add_entry("README.TXT", UINT32_MAX, "UMDos 64-bit kernel\nOpen DOCS and SYSTEM with a double click.\n");
    add_entry("MOTD.TXT", UINT32_MAX, "A bosom friend afar brings a distant land near.\n");
    add_entry("NOTES.TXT", UINT32_MAX, "Click 'My Computer' to open a file manager window.\n");
    add_entry("CONTACT.TXT", UINT32_MAX, "UMDos demo system\nMail: umdos@example.invalid\n");
    u32 docs = add_entry("DOCS", UINT32_MAX, NULL);
    add_entry("INTRO.TXT", docs, "Double click a directory to open it. Press Backspace to go up.\n");
    add_entry("CHANGELOG.TXT", docs, "UMDFS supports writes, subdirectories and ATA write-back.\n");
    u32 system = add_entry("SYSTEM", UINT32_MAX, NULL);
    add_entry("SYSTEM.TXT", system, "BIOS boot chain: boot -> loader -> head -> 64-bit kernel\n");
    add_entry("SOURCES.TXT", system, "Kernel sources are in kernel/.\n");
    u32 config = add_entry("CONFIG", UINT32_MAX, NULL);
    add_entry("AUTH", config, auth);

    memcpy(fs, "UMDFS1", 6);
    const u32 fields[] = {SECTOR, FS_SECTORS, entries, DATA_BLOCK, DIR_BLOCK,
                          DIR_COUNT, BITMAP_BLOCK, UINT32_C(0x46533032)};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        put32(fs + 8 + i * 4, fields[i]);
    for (u32 block = 0; block < DATA_BLOCK; ++block)
        mark_block(block);

    u32 hash = fnv1a(UINT32_C(2166136261), fs, 40);
    hash = fnv1a(hash, fs + DIR_BLOCK * SECTOR, DIR_COUNT * DIR_SIZE);
    hash = fnv1a(hash, fs + BITMAP_BLOCK * SECTOR, SECTOR);
    put32(fs + 40, hash);
    return 1;
}

static int read_stage(const char *name, u32 lba, size_t capacity, int exact)
{
    FILE *file = fopen(name, "rb");
    if (!file) {
        perror(name);
        return 0;
    }
    size_t count = fread(disk + lba * SECTOR, 1, capacity, file);
    int valid = fgetc(file) == EOF && !ferror(file) && count != 0 &&
                (!exact || count == capacity);
    fclose(file);
    if (!valid)
        fprintf(stderr, "%s: invalid stage size (expected %s%zu bytes)\n",
                name, exact ? "" : "at most ", capacity);
    return valid;
}

static int write_image(const char *name, const u8 *data, size_t size)
{
    FILE *file = fopen(name, "wb");
    if (!file) {
        perror(name);
        return 0;
    }
    size_t written = fwrite(data, 1, size, file);
    int closed = fclose(file);
    if (written == size && closed == 0)
        return 1;
    fprintf(stderr, "%s: could not write image\n", name);
    remove(name);
    return 0;
}

int main(void)
{
    if (!read_stage("boot.bin", 0, SECTOR, 1) ||
        !read_stage("loader.bin", 1, 8 * SECTOR, 1) ||
        !read_stage("head.bin", 9, 4 * SECTOR, 1) ||
        !read_stage("kernel.bin", 50, 320 * SECTOR, 0) ||
        !build_fs())
        return 1;

    const u8 empty_partition_table[64] = {0};
    if (memcmp(disk + 446, empty_partition_table, sizeof(empty_partition_table)) != 0 ||
        disk[510] != 0x55 || disk[511] != 0xaa) {
        fputs("boot.bin: expected an empty MBR table and a boot signature\n", stderr);
        return 1;
    }
    if (!write_image("fs.img", fs, FS_SECTORS * SECTOR) ||
        !write_image("UMDos-floppy.img", disk, FLOPPY_SECTORS * SECTOR))
        return 1;

    /* Preserve the legacy HDD image's active MBR entry. */
    const u8 partition[8] = {0x80, 0, 1, 1, 0x0c, 254, 255, 255};
    memcpy(disk + 446, partition, sizeof(partition));
    put32(disk + 454, FS_LBA);
    put32(disk + 458, 2048);
    return write_image("UMDos-hdd.img", disk, sizeof(disk)) ? 0 : 1;
}
