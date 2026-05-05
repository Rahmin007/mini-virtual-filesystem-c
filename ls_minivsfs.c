// Tiny reader to list root directory of a MiniVSFS image
// Build: gcc -O2 -std=c17 -Wall -Wextra ls_minivsfs.c -o ls_minivsfs
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BS 4096u
#define DIRECT_MAX 12

#pragma pack(push,1)
typedef struct {
    uint32_t magic; uint32_t version; uint32_t block_size; uint64_t total_blocks;
    uint64_t inode_count; uint64_t inode_bitmap_start; uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;  uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;  uint64_t inode_table_blocks;
    uint64_t data_region_start;  uint64_t data_region_blocks;
    uint64_t root_inode; uint64_t mtime_epoch; uint32_t flags; uint32_t checksum;
} superblock_t;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct {
    uint16_t mode; uint16_t links; uint32_t uid; uint32_t gid;
    uint64_t size_bytes, atime, mtime, ctime;
    uint32_t direct[DIRECT_MAX];
    uint32_t r0, r1, r2; uint32_t proj_id; uint32_t uid16_gid16; uint64_t xattr_ptr; uint64_t inode_crc;
} inode_t;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct { 
    uint32_t inode_no; 
    uint8_t type; 
    char name[58]; 
    uint8_t checksum; 
} dirent64_t;
#pragma pack(pop)

int main(int argc, char **argv){
    if(argc!=2){ 
        fprintf(stderr,"Usage: %s image.img\n", argv[0]); 
        return 1; 
    }
    const char *path=argv[1];
    FILE *f=fopen(path,"rb"); 
    if(!f){ perror("fopen"); return 1; }

    // read superblock
    superblock_t sb; 
    if(fread(&sb,1,sizeof(sb),f)!=sizeof(sb)){ perror("read sb"); fclose(f); return 1; }
    if(sb.magic!=0x4D565346u || sb.version!=1u || sb.block_size!=BS){
        fprintf(stderr,"Not a MiniVSFS image or unsupported version.\n"); 
        fclose(f); 
        return 1; 
    }

    // locate inode table and root inode
    if(fseek(f, (long)(sb.inode_table_start*BS), SEEK_SET)!=0){ perror("fseek itbl"); fclose(f); return 1; }
    inode_t root; 
    if(fread(&root,1,sizeof(root),f)!=sizeof(root)){ perror("read inode0"); fclose(f); return 1; }

    // read first data block of root (its directory page)
    if(root.direct[0]==0){ 
        fprintf(stderr,"Root has no data block.\n"); 
        fclose(f); 
        return 1; 
    }
    if(fseek(f, (long)(root.direct[0]*BS), SEEK_SET)!=0){ perror("fseek dir"); fclose(f); return 1; }

    const size_t nentries = BS/sizeof(dirent64_t);
    dirent64_t *buf = (dirent64_t*)malloc(BS); 
    if(!buf){ perror("malloc"); fclose(f); return 1; }
    if(fread(buf,1,BS,f)!=BS){ perror("read dir block"); free(buf); fclose(f); return 1; }

    printf("/ (inode #1) contains:\n");
    for(size_t i=0;i<nentries;i++){
        if(buf[i].inode_no==0) continue; // empty slot
        char name[59]; memcpy(name, buf[i].name, 58); name[58]='\0';
        // trim trailing NULs/spaces
        for(int j=57;j>=0;j--){ if(name[j]=='\0' || name[j]==' ') name[j]='\0'; else break; }
        printf("  [%s] inode=%u type=%s\n", name, buf[i].inode_no, buf[i].type==2?"dir":"file");
    }

    free(buf); 
    fclose(f); 
    return 0;
}

