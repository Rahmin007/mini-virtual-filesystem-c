// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c -o mkfs_adder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <stdbool.h>

#define BS 4096u
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

// =============================== Helpers =====================================
static inline int bm_get(const uint8_t *bm, uint64_t idx){ return (bm[idx>>3] >> (idx & 7u)) & 1u; }
static inline void bm_set(uint8_t *bm, uint64_t idx){ bm[idx>>3] |= (uint8_t)(1u << (idx & 7u)); }
static const char* base_name(const char *p){ const char *s=strrchr(p,'/'); return s? s+1 : p; }

int main(int argc, char **argv) {
    crc32_init();

    const char *in_path=NULL, *out_path=NULL, *file_path=NULL;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--input") && i+1<argc) in_path=argv[++i];
        else if(!strcmp(argv[i],"--output") && i+1<argc) out_path=argv[++i];
        else if(!strcmp(argv[i],"--file") && i+1<argc) file_path=argv[++i];
        else {
            fprintf(stderr,
                "Usage: %s --input IN.img --output OUT.img --file FILE\n", argv[0]);
            return 1;
        }
    }
    if(!in_path || !out_path || !file_path){
        fprintf(stderr,
            "Usage: %s --input IN.img --output OUT.img --file FILE\n", argv[0]);
        return 1;
    }

    // Read entire input image
    FILE *fi=fopen(in_path,"rb"); if(!fi){ perror("fopen input"); return 1; }
    if(fseek(fi,0,SEEK_END)!=0){ perror("fseek"); fclose(fi); return 1; }
    long long flen = ftell(fi); if(flen<0){ perror("ftell"); fclose(fi); return 1; }
    if(fseek(fi,0,SEEK_SET)!=0){ perror("fseek"); fclose(fi); return 1; }
    uint8_t *img=(uint8_t*)calloc(1,(size_t)flen); if(!img){ perror("calloc"); fclose(fi); return 1; }
    if(fread(img,1,(size_t)flen,fi)!=(size_t)flen){ perror("fread"); free(img); fclose(fi); return 1; }
    fclose(fi);

    if((flen % BS)!=0){ fprintf(stderr,"Input image not multiple of block size\n"); free(img); return 1; }

    // Validate superblock
    superblock_t *sb=(superblock_t*)img; // block 0
    if(sb->magic!=0x4D565346u){ fprintf(stderr,"Bad magic\n"); free(img); return 1; }
    if(sb->version!=1u || sb->block_size!=BS){ fprintf(stderr,"Unsupported version/block size\n"); free(img); return 1; }
    {
        uint32_t old=sb->checksum; sb->checksum=0; uint32_t exp = crc32(sb, BS-4); sb->checksum=old;
        if(exp!=old) fprintf(stderr,"Warning: superblock checksum mismatch (exp=%08x got=%08x)\n", exp, old);
    }

    uint8_t *ibm = img + (size_t)sb->inode_bitmap_start * BS;
    uint8_t *dbm = img + (size_t)sb->data_bitmap_start  * BS;
    inode_t *itbl = (inode_t*)(img + (size_t)sb->inode_table_start * BS);

    // Load host file
    struct stat st; if(stat(file_path,&st)!=0){ perror("stat file"); free(img); return 1; }
    if(!S_ISREG(st.st_mode)){ fprintf(stderr,"--file must be a regular file\n"); free(img); return 1; }
    uint64_t fsize=(uint64_t)st.st_size;
    uint32_t need = (uint32_t)((fsize + BS - 1)/BS);
    if(need==0) need=1; // treat empty file as 1 block for simplicity
    if(need>DIRECT_MAX){ fprintf(stderr,"File too large: needs %u blocks (>12)\n", need); free(img); return 1; }

    // Find free inode (skip index 0 -> inode #1 is root)
    uint64_t free_ino_idx=(uint64_t)-1; // 0-based index into table
    for(uint64_t i=1;i<sb->inode_count;i++) if(!bm_get(ibm,i)){ free_ino_idx=i; break; }
    if(free_ino_idx==(uint64_t)-1){ fprintf(stderr,"No free inode available\n"); free(img); return 1; }
    bm_set(ibm, free_ino_idx);
    uint32_t new_ino_no = (uint32_t)(free_ino_idx + 1u);

    // Find free data blocks
    uint32_t chosen[DIRECT_MAX]; memset(chosen,0,sizeof(chosen));
    uint32_t picked=0;
    for(uint64_t i=0;i<sb->data_region_blocks && picked<need;i++){
        if(!bm_get(dbm,i)){
            bm_set(dbm,i);
            chosen[picked++] = (uint32_t)(sb->data_region_start + i);
        }
    }
    if(picked!=need){ fprintf(stderr,"Not enough free data blocks\n"); free(img); return 1; }

    // Prepare inode for the new file
    inode_t *node = &itbl[free_ino_idx];
    memset(node,0,sizeof(*node));
    node->mode = 0100000u; // file
    node->links=1;
    node->uid=0; node->gid=0;
    node->size_bytes=fsize;
    node->atime=node->mtime=node->ctime=(uint64_t)time(NULL);
    for(uint32_t i=0;i<DIRECT_MAX;i++) node->direct[i]=0;
    for(uint32_t i=0;i<need;i++) node->direct[i]=chosen[i];
    node->proj_id = 12; // Group 12
    node->uid16_gid16 = 0; node->xattr_ptr=0;
    inode_crc_finalize(node);

    // Write file data to chosen blocks
    FILE *ff=fopen(file_path,"rb"); if(!ff){ perror("fopen file"); free(img); return 1; }
    uint8_t *blk=(uint8_t*)calloc(1,BS); if(!blk){ perror("calloc blk"); fclose(ff); free(img); return 1; }
    uint64_t remain=fsize; for(uint32_t i=0;i<need;i++){
        memset(blk,0,BS);
        size_t want = (remain>BS)? BS : (size_t)remain;
        if(want>0){ if(fread(blk,1,want,ff)!=want){ perror("fread file"); free(blk); fclose(ff); free(img); return 1; } }
        memcpy(img + (size_t)chosen[i]*BS, blk, BS);
        remain = (remain>BS)? (remain-BS):0;
    }
    free(blk); fclose(ff);

    // Update root directory (first block of root->direct[0])
    inode_t *root = &itbl[0];
    dirent64_t *rootdir = (dirent64_t*)(img + (size_t)root->direct[0]*BS);
    const uint32_t nslots = BS/sizeof(dirent64_t);
    uint32_t slot=(uint32_t)-1; for(uint32_t i=0;i<nslots;i++){ if(rootdir[i].inode_no==0){ slot=i; break; } }
    if(slot==(uint32_t)-1){ fprintf(stderr,"Root directory full\n"); free(img); return 1; }

    dirent64_t de; memset(&de,0,sizeof(de));
    de.inode_no=new_ino_no; de.type=1; // file
    const char *bn = base_name(file_path);
    size_t name_len=strlen(bn); if(name_len>58) name_len=58;
    memset(de.name,0,58); memcpy(de.name,bn,name_len);
    dirent_checksum_finalize(&de);
    rootdir[slot]=de;

    // bump root links and re-CRC
    root->links += 1;
    root->mtime = (uint64_t)time(NULL);
    inode_crc_finalize(root);

    // refresh superblock time + checksum
    sb->mtime_epoch = (uint64_t)time(NULL);
    superblock_crc_finalize(sb);

    // Write output image
    FILE *fo=fopen(out_path,"wb"); if(!fo){ perror("fopen output"); free(img); return 1; }
    if(fwrite(img,1,(size_t)flen,fo)!=(size_t)flen){ perror("fwrite output"); fclose(fo); free(img); return 1; }
    fclose(fo); free(img);

    fprintf(stdout,"Added '%s' as inode #%u into %s -> %s\n", bn, new_ino_no, in_path, out_path);
    return 0;
}

