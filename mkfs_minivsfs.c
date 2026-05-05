// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

// ===================== On-disk metadata structs =====================
#pragma pack(push, 1)
typedef struct {
    // Superblock: EXACTLY 116 bytes; checksum MUST be last
    uint32_t magic;              // 'MVSF' = 0x4D565346
    uint32_t version;            // 1
    uint32_t block_size;         // 4096
    uint64_t total_blocks;       // image_size / block_size

    uint64_t inode_count;        // # of inodes
    uint64_t inode_bitmap_start; // block idx of inode bitmap (1)
    uint64_t inode_bitmap_blocks;// 1

    uint64_t data_bitmap_start;  // block idx of data bitmap (2)
    uint64_t data_bitmap_blocks; // 1

    uint64_t inode_table_start;  // block idx of inode table (3)
    uint64_t inode_table_blocks; // ceil(inode_count*INODE_SIZE/BS)

    uint64_t data_region_start;  // first data block idx
    uint64_t data_region_blocks; // remaining blocks

    uint64_t root_inode;         // 1
    uint64_t mtime_epoch;        // Unix epoch

    uint32_t flags;              // 0
    uint32_t checksum;           // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    // Inode: EXACTLY 128 bytes; inode_crc MUST be last 8 bytes
    uint16_t mode;      // 040000 dir, 0100000 file (octal)
    uint16_t links;     // link count
    uint32_t uid;       // 0
    uint32_t gid;       // 0

    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;

    uint32_t direct[DIRECT_MAX]; // absolute block numbers (0 if unused)

    uint32_t reserved_0; // 0
    uint32_t reserved_1; // 0
    uint32_t reserved_2; // 0

    uint32_t proj_id;      // group id (set to 12)
    uint32_t uid16_gid16;  // 0
    uint64_t xattr_ptr;    // 0

    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    // Directory entry: EXACTLY 64 bytes; checksum MUST be last
    uint32_t inode_no;  // 0 if free
    uint8_t  type;      // 1=file, 2=dir
    char     name[58];  // not necessarily NUL terminated
    uint8_t  checksum;  // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

// ========================== DO NOT CHANGE THIS PORTION =========================
// CRC32 helpers (provided)
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}
// WARNING: CALL THIS ONLY AFTER ALL OTHER INODE ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}
// WARNING: CALL THIS ONLY AFTER ALL OTHER DIRENT ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}
// ======================== END DO-NOT-TOUCH SECTION ===========================

// ================================ Utilities =================================
static inline uint64_t ceil_div_u64(uint64_t a, uint64_t b){ return (a + b - 1) / b; }
static inline void bm_set(uint8_t *bm, uint64_t idx){ bm[idx>>3] |= (uint8_t)(1u << (idx & 7u)); }

// ================================== MAIN =====================================
int main(int argc, char **argv){
    crc32_init();

    const char *out_path=NULL; uint64_t size_kib=0, inodes=0; uint64_t seed=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--image") && i+1<argc) out_path=argv[++i];
        else if(!strcmp(argv[i],"--size-kib") && i+1<argc) size_kib = strtoull(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--inodes") && i+1<argc) inodes = strtoull(argv[++i],NULL,10);
        else if(!strcmp(argv[i],"--seed") && i+1<argc) seed = strtoull(argv[++i],NULL,10);
        else {
            fprintf(stderr,
                "Usage: %s --image OUT.img --size-kib N --inodes M [--seed S]\n", argv[0]);
            return 1;
        }
    }
    if(!out_path || !size_kib || !inodes){
        fprintf(stderr,
            "Usage: %s --image OUT.img --size-kib N --inodes M [--seed S]\n", argv[0]);
        return 1;
    }
    if(size_kib < 180 || size_kib > 4096 || (size_kib % 4)!=0){
        fprintf(stderr,"--size-kib must be in [180..4096] and multiple of 4\n");
        return 1;
    }
    if(inodes < 128 || inodes > 512){
        fprintf(stderr,"--inodes must be in [128..512]\n");
        return 1;
    }
    (void)seed; // accepted but not used

    // ----- Geometry -----
    uint64_t total_blocks = (size_kib * 1024ull) / BS;
    uint64_t inode_table_bytes = inodes * INODE_SIZE;
    uint64_t inode_table_blocks = ceil_div_u64(inode_table_bytes, BS);

    uint64_t sb_start   = 0;
    uint64_t ibm_start  = 1;
    uint64_t dbm_start  = 2;
    uint64_t it_start   = 3;
    uint64_t data_start = it_start + inode_table_blocks;
    if(data_start >= total_blocks){ fprintf(stderr,"Invalid geometry: no room for data region\n"); return 1; }
    uint64_t data_blocks = total_blocks - data_start;

    size_t img_len = (size_t)total_blocks * BS;
    uint8_t *img = (uint8_t*)calloc(1, img_len);
    if(!img){ perror("calloc image"); return 1; }

    // ----- Pointers -----
    superblock_t *sb = (superblock_t*)(img + sb_start*BS);
    uint8_t *ibm = img + ibm_start*BS;
    uint8_t *dbm = img + dbm_start*BS;
    inode_t *itbl = (inode_t*)(img + it_start*BS);
    uint8_t *data = img + data_start*BS;

    // ----- Superblock -----
    memset(sb,0,sizeof(*sb));
    sb->magic = 0x4D565346u;
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = inodes;
    sb->inode_bitmap_start = ibm_start;
    sb->inode_bitmap_blocks = 1;
    sb->data_bitmap_start = dbm_start;
    sb->data_bitmap_blocks = 1;
    sb->inode_table_start = it_start;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = data_start;
    sb->data_region_blocks = data_blocks;
    sb->root_inode = ROOT_INO;
    sb->mtime_epoch = (uint64_t)time(NULL);
    sb->flags = 0;
    superblock_crc_finalize(sb);

    // ----- Bitmaps -----
    memset(ibm,0,BS); memset(dbm,0,BS);
    // Mark root inode (#1) and first data block for root directory
    bm_set(ibm, 0); // inode index 0 => inode #1 allocated
    bm_set(dbm, 0); // first data block in data region allocated for root dir

    // ----- Inode table / Root inode -----
    memset(itbl,0,(size_t)inode_table_blocks*BS);
    inode_t *root = &itbl[0];
    root->mode  = 0040000u; // directory
    root->links = 2;        // '.' and '..'
    root->uid = 0; root->gid = 0;
    root->size_bytes = BS;  // one directory block reserved
    root->atime = root->mtime = root->ctime = (uint64_t)time(NULL);
    for(int i=0;i<DIRECT_MAX;i++) root->direct[i]=0;
    root->direct[0] = (uint32_t)data_start; // absolute block number of first data block
    root->reserved_0 = root->reserved_1 = root->reserved_2 = 0;
    root->proj_id = 12; // Group 12 tag
    root->uid16_gid16 = 0;
    root->xattr_ptr = 0;
    inode_crc_finalize(root);

    // ----- Root directory block -----
    dirent64_t *dir = (dirent64_t*)data; // first data block
    memset(dir,0,BS);

    dirent64_t de; memset(&de,0,sizeof(de));
    // "."
    de.inode_no = ROOT_INO; de.type = 2; memset(de.name,0,58); memcpy(de.name, ".", 1); dirent_checksum_finalize(&de); dir[0]=de;
    // ".."
    memset(&de,0,sizeof(de));
    de.inode_no = ROOT_INO; de.type = 2; memset(de.name,0,58); memcpy(de.name, "..", 2); dirent_checksum_finalize(&de); dir[1]=de;

    // ----- Save image -----
    FILE *fo=fopen(out_path,"wb"); if(!fo){ perror("fopen output"); free(img); return 1; }
    if(fwrite(img,1,img_len,fo)!=img_len){ perror("fwrite output"); fclose(fo); free(img); return 1; }
    fclose(fo); free(img);

    fprintf(stdout,
        "Created %s: blocks=%" PRIu64 ", inode_table_blocks=%" PRIu64 ", data_start=%" PRIu64 ", inodes=%" PRIu64 "\n",
        out_path, total_blocks, inode_table_blocks, data_start, inodes);
    return 0;
}

