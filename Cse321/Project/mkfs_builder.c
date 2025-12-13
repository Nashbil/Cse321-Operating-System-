\
/*
 Build:
   gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder

 Usage:
   ./mkfs_builder --image out.img --size-kib <180..4096> --inodes <128..512>
*/
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>   // for ssize_t, off_t

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

// ============================ Helpers / CRC ============================
#pragma pack(push,1)
typedef struct {
    uint32_t magic;                 // 0x4D565346 "MVFS"
    uint32_t version;               // 1
    uint32_t block_size;            // 4096
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;   // 1
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;    // 1
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;            // 1
    uint64_t mtime_epoch;
    uint32_t flags;                 // 0
    uint32_t checksum;              // CRC32 over struct with checksum=0
} superblock_t;

typedef struct {
    uint16_t mode;                 // 0100000 file, 0040000 dir (octal)
    uint16_t links;                // 2 for root, 1 for files
    uint32_t uid;                  // 4
    uint32_t gid;                  // 4
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];   // 12 direct block pointers (absolute)
    uint32_t reserved_0;           // 0
    uint32_t reserved_1;           // 0
    uint32_t reserved_2;           // 0
    uint32_t proj_id;              // group id 14 ; 
    uint32_t uid16_gid16;          // 0
    uint64_t xattr_ptr;            // 0
    uint64_t inode_crc;            // CRC32 over struct with inode_crc=0
} inode_t;

typedef struct {
    uint32_t inode_no;             // 0 if free
    uint8_t  type;                 // 1=file, 2=dir
    char     name[58];             // NUL-terminated if shorter
    uint8_t  checksum;             // CRC8 (low byte of CRC32)
} dirent64_t;
#pragma pack(pop)

_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode size mismatch");
_Static_assert(sizeof(dirent64_t) == 64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
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
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

// ============================ Bitmap helpers ============================
static inline void bitmap_set(uint8_t* bm, size_t idx){
    bm[idx>>3] |= (uint8_t)(1u << (idx & 7u));
}
static inline void bitmap_clear(uint8_t* bm, size_t idx){
    bm[idx>>3] &= (uint8_t)~(1u << (idx & 7u));
}
static inline int bitmap_test(const uint8_t* bm, size_t idx){
    return (bm[idx>>3] >> (idx & 7u)) & 1u;
}

// ============================ CLI ============================
static void usage(const char* prog){
    fprintf(stderr,
        "Usage: %s --image <out.img> --size-kib <180..4096> --inodes <128..512>\n",
        prog);
}

int main(int argc, char** argv){
    crc32_init();
    const char* image = NULL;
    long size_kib = -1;
    long inode_count = -1;
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i], "--image") && i+1<argc){ image = argv[++i]; }
        else if (!strcmp(argv[i], "--size-kib") && i+1<argc){ size_kib = strtol(argv[++i], NULL, 10); }
        else if (!strcmp(argv[i], "--inodes") && i+1<argc){ inode_count = strtol(argv[++i], NULL, 10); }
        else { usage(argv[0]); return 2; }
    }
    if (!image || size_kib<180 || size_kib>4096 || (size_kib%4)!=0 ||
        inode_count<128 || inode_count>512){
        usage(argv[0]);
        return 2;
    }

    const uint64_t total_blocks = ((uint64_t)size_kib * 1024u) / BS;
    if (total_blocks <  (1+1+1+1)) { fprintf(stderr, "image too small\n"); return 2; }

    // Compute layout
    const uint64_t inode_table_blocks = ( (inode_count*INODE_SIZE) + (BS-1) ) / BS;
    const uint64_t inode_bitmap_start = 1;
    const uint64_t data_bitmap_start  = 2;
    const uint64_t inode_table_start  = 3;
    const uint64_t data_region_start  = inode_table_start + inode_table_blocks;
    if (data_region_start >= total_blocks){ fprintf(stderr,"invalid layout\n"); return 2; }
    const uint64_t data_region_blocks = total_blocks - data_region_start;

    // Allocate in-memory image pieces
    uint8_t* inode_bm = calloc(1, BS);
    uint8_t* data_bm  = calloc(1, BS);
    inode_t* itab     = calloc(inode_count, sizeof(inode_t));
    uint8_t* data     = calloc(data_region_blocks, BS);
    if (!inode_bm || !data_bm || !itab || !data){
        fprintf(stderr, "oom\n"); return 1;
    }

    // Root inode allocation
    bitmap_set(inode_bm, 0); // inode #1
    // Root data block allocation (first data block in data region)
    bitmap_set(data_bm, 0);
    // Prepare root inode
    time_t now = time(NULL);
    inode_t* root = &itab[0];
    memset(root, 0, sizeof(*root));
    root->mode  = 0040000;  // directory
    root->links = 2;        // . and ..
    root->uid = 0; root->gid = 0;
    root->size_bytes = 2 * sizeof(dirent64_t);
    root->atime = root->mtime = root->ctime = (uint64_t)now;
    root->direct[0] = (uint32_t)(data_region_start + 0);
    for (int i=1;i<DIRECT_MAX;i++) root->direct[i]=0;
    root->proj_id = 14; root->uid16_gid16=0; root->xattr_ptr=0; // project id set 
    inode_crc_finalize(root);

    // Prepare root directory block with "." and ".."
    dirent64_t dot = {0}, dotdot = {0};
    dot.inode_no = ROOT_INO; dot.type = 2;
    strncpy(dot.name, ".", sizeof(dot.name)-1);
    dirent_checksum_finalize(&dot);
    dotdot.inode_no = ROOT_INO; dotdot.type = 2;
    strncpy(dotdot.name, "..", sizeof(dotdot.name)-1);
    dirent_checksum_finalize(&dotdot);

    // Write into first data block
    uint8_t* blk0 = data + 0*BS;
    memcpy(blk0 + 0*sizeof(dirent64_t), &dot, sizeof(dot));
    memcpy(blk0 + 1*sizeof(dirent64_t), &dotdot, sizeof(dotdot));
    // rest remains zero -> free entries

    // Superblock
    superblock_t sb = {0};
    sb.magic = 0x4D565346u; // 'M''V''S''F'
    sb.version = 1;
    sb.block_size = BS;
    sb.total_blocks = total_blocks;
    sb.inode_count = (uint64_t)inode_count;
    sb.inode_bitmap_start = inode_bitmap_start;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = data_bitmap_start;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = inode_table_start;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_region_start = data_region_start;
    sb.data_region_blocks = data_region_blocks;
    sb.root_inode = ROOT_INO;
    sb.mtime_epoch = (uint64_t)now;
    sb.flags = 0;
    sb.checksum = 0;
    superblock_crc_finalize(&sb);

    // Write image
    FILE* f = fopen(image, "wb");
    if (!f){ perror("fopen"); return 1; }

    // block 0: superblock
    uint8_t zero[BS]; memset(zero,0,sizeof(zero));
    uint8_t sbpad[BS]; memset(sbpad,0,sizeof(sbpad));
    memcpy(sbpad, &sb, sizeof(sb));
    if (fwrite(sbpad,1,BS,f)!=BS){ perror("write sb"); return 1; }

    // block 1: inode bitmap
    if (fwrite(inode_bm,1,BS,f)!=BS){ perror("write ibm"); return 1; }

    // block 2: data bitmap
    if (fwrite(data_bm,1,BS,f)!=BS){ perror("write dbm"); return 1; }

    // inode table
    size_t it_bytes = inode_count * sizeof(inode_t);
    if (fwrite(itab,1,it_bytes,f)!=it_bytes){ perror("write itab"); return 1; }
    size_t it_pad = (size_t)(inode_table_blocks*BS - it_bytes);
    if (it_pad){
        memset(zero,0,BS);
        while (it_pad){
            size_t chunk = it_pad>BS?BS:it_pad;
            if (fwrite(zero,1,chunk,f)!=chunk){ perror("itab pad"); return 1; }
            it_pad -= chunk;
        }
    }

    // data region
    size_t data_bytes = (size_t)(data_region_blocks*BS);
    if (data_bytes){
        if (fwrite(data,1,data_bytes,f)!=data_bytes){ perror("write data"); return 1; }
    }

    fclose(f);
    free(inode_bm);
    free(data_bm);
    free(itab);
    free(data);

    fprintf(stdout, "Created MiniVSFS image '%s' : %lu KiB, %ld inodes, %lu blocks, data region starts at #%lu\n",
        image, (unsigned long)size_kib, inode_count, (unsigned long)total_blocks, (unsigned long)data_region_start);
    return 0;
}
