#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push,1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;

typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;

typedef struct {
    uint32_t inode_no;
    uint8_t  type;        // 1=file, 2=dir
    char     name[58];
    uint8_t  checksum;
} dirent64_t;
#pragma pack(pop)

_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

// ========================== DO NOT CHANGE THIS PORTION =========================
// CRC32 helpers
static uint32_t CRC32_TAB[256];
static void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for (int j=0;j<8;j++){
            c = (c&1)? (0xEDB88320u ^ (c>>1)) : (c>>1);
        }
        CRC32_TAB[i]=c;
    }
}
static uint32_t crc32_update(uint32_t crc, const void*buf, size_t len){
    const uint8_t* p=(const uint8_t*)buf;
    for (size_t i=0;i<len;i++){
        crc = CRC32_TAB[(crc ^ p[i]) & 0xFFu] ^ (crc>>8);
    }
    return crc;
}
static uint32_t crc32_finalize(const void*buf, size_t len){
    uint32_t c = 0xFFFFFFFFu;
    c = crc32_update(c, buf, len);
    return c ^ 0xFFFFFFFFu;
}

// Superblock CRC
static void superblock_crc_finalize(superblock_t* sb){
    uint32_t saved = sb->checksum;
    sb->checksum = 0;
    sb->checksum = crc32_finalize(sb, sizeof(*sb));
    (void)saved;
}

// Inode CRC
static void inode_crc_finalize(inode_t* in){
    uint64_t saved = in->inode_crc;
    in->inode_crc = 0;
    uint32_t c = crc32_finalize(in, sizeof(*in));
    in->inode_crc = (uint64_t)c;
    (void)saved;
}

// Dirent checksum (xor of first 63 bytes)
static void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

// ================= Bitmap helpers =================
static inline void bitmap_set(uint8_t* bm, size_t idx){ bm[idx>>3] |= (uint8_t)(1u << (idx & 7u)); }
static inline void bitmap_clear(uint8_t* bm, size_t idx){ bm[idx>>3] &= (uint8_t)~(1u << (idx & 7u)); }
static inline int  bitmap_test(const uint8_t* bm, size_t idx){ return (bm[idx>>3] >> (idx & 7u)) & 1u; }
static long long bitmap_ffz(const uint8_t* bm, size_t bits){
    for (size_t i=0;i<bits;i++){ if (!bitmap_test(bm,i)) return (long long)i; }
    return -1;
}

// ================= CLI =================
static void usage(const char* prog){
    fprintf(stderr, "Usage: %s --input in.img --output out.img --file <path>\n", prog);
}

int main(int argc, char** argv){
    crc32_init();

    const char* inpath=NULL;
    const char* outpath=NULL;
    const char* filepath=NULL;

    // Simple manual CLI parsing
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--input") && i+1<argc) inpath = argv[++i];
        else if (!strcmp(argv[i],"--output") && i+1<argc) outpath = argv[++i];
        else if (!strcmp(argv[i],"--file") && i+1<argc) filepath = argv[++i];
        else { usage(argv[0]); return 2; }
    }
    if (!inpath || !outpath || !filepath){ usage(argv[0]); return 2; }

    // Read whole image
    FILE* fi = fopen(inpath, "rb");
    if (!fi){ perror("open input"); return 1; }
    if (fseek(fi, 0, SEEK_END) != 0){ perror("seek input"); fclose(fi); return 1; }
    long isz = ftell(fi);
    if (isz <= 0){ fprintf(stderr, "empty image\n"); fclose(fi); return 1; }
    if (fseek(fi, 0, SEEK_SET) != 0){ perror("rewind input"); fclose(fi); return 1; }

    uint8_t* img = (uint8_t*)malloc((size_t)isz);
    if (!img){ fprintf(stderr,"oom\n"); fclose(fi); return 1; }
    if (fread(img,1,(size_t)isz,fi)!=(size_t)isz){ perror("read image"); fclose(fi); free(img); return 1; }
    fclose(fi);

    // Map structures
    superblock_t* sb = (superblock_t*)(img + 0);
    if (sb->block_size != BS || sb->magic != 0x4D565346u){
        fprintf(stderr,"not a MiniVSFS image\n");
        free(img);
        return 2;
    }
    uint8_t* inode_bm = img + (size_t)sb->inode_bitmap_start * BS;
    uint8_t* data_bm  = img + (size_t)sb->data_bitmap_start  * BS;
    inode_t* itab     = (inode_t*)(img + (size_t)sb->inode_table_start * BS);

    // Read file to add
    FILE* ff = fopen(filepath, "rb");
    if (!ff){ perror("open file"); free(img); return 1; }
    if (fseek(ff, 0, SEEK_END) != 0){ perror("seek file"); fclose(ff); free(img); return 1; }
    long fsz = ftell(ff);
    if (fsz < 0){ fprintf(stderr,"file size error\n"); fclose(ff); free(img); return 1; }
    if (fseek(ff, 0, SEEK_SET) != 0){ perror("rewind file"); fclose(ff); free(img); return 1; }

    uint8_t* fbuf = NULL;
    if (fsz > 0){
        fbuf = (uint8_t*)malloc((size_t)fsz);
        if (!fbuf){ fprintf(stderr,"oom\n"); fclose(ff); free(img); return 1; }
        if (fread(fbuf,1,(size_t)fsz,ff)!=(size_t)fsz){ perror("read file"); fclose(ff); free(fbuf); free(img); return 1; }
    }
    fclose(ff);

    // Find free inode
    long long free_in = bitmap_ffz(inode_bm, (size_t)sb->inode_count);
    if (free_in < 0){ fprintf(stderr,"no free inode available\n"); free(fbuf); free(img); return 1; }
    if ((size_t)free_in >= sb->inode_count){ fprintf(stderr,"inode index OOB\n"); free(fbuf); free(img); return 1; }
    uint32_t new_ino = (uint32_t)(free_in + 1); // 1-indexed

    // Blocks needed
    uint64_t blocks_needed = ((uint64_t)fsz + (BS-1)) / BS;
    if (blocks_needed > DIRECT_MAX){
        fprintf(stderr,"Error: file too large for MiniVSFS (needs %llu blocks, max %d / %d KiB)\n",
                (unsigned long long)blocks_needed, DIRECT_MAX, DIRECT_MAX*(BS/1024));
        free(fbuf); free(img); return 1;
    }

    // Allocate data blocks (first-fit)
    uint32_t direct[DIRECT_MAX] = {0};
    for (uint64_t i=0;i<blocks_needed;i++){
        long long idx = bitmap_ffz(data_bm, (size_t)sb->data_region_blocks);
        if (idx < 0){
            fprintf(stderr,"no free data blocks\n");
            free(fbuf); free(img); return 1;
        }
        bitmap_set(data_bm, (size_t)idx);
        direct[i] = (uint32_t)(sb->data_region_start + (uint64_t)idx);
    }

    // Root directory block pointer
    inode_t* root = &itab[0];
    uint32_t first_dir_block = root->direct[0];
    if (!first_dir_block){
        fprintf(stderr,"root missing first data block\n");
        free(fbuf); free(img); return 1;
    }
    uint8_t* dblk = img + (size_t)first_dir_block * BS;
    dirent64_t* dent = (dirent64_t*)dblk;
    size_t entries = BS / sizeof(dirent64_t);

    // Basename of the file (used for dup check + dirent + final printf)
    const char* base = strrchr(filepath, '/');
    base = base ? base+1 : filepath;

    // Duplicate filename check & find free slot
    long long slot = -1;
    size_t used_entries = 0;
    for (size_t i=0;i<entries; i++){
        if (dent[i].inode_no != 0){
            used_entries++;
            if (strncmp(dent[i].name, base, sizeof(dent[i].name)) == 0){
                fprintf(stderr, "Error: file '%s' already exists in root directory.\n", base);
                free(fbuf); free(img); return 1;
            }
        } else if (slot < 0){
            slot = (long long)i;
        }
    }
    if (slot < 0){
        fprintf(stderr, "Error: root directory is full (max ~%zu files including . and ..).\n", entries);
        free(fbuf); free(img); return 1;
    }

    // Create inode for the new file
    time_t now = time(NULL);
    inode_t* inode = &itab[free_in];
    memset(inode, 0, sizeof(*inode));
    inode->mode = 0100000;       // file
    inode->links = 1;
    inode->uid = 0;
    inode->gid = 0;
    inode->size_bytes = (uint64_t)fsz;
    inode->proj_id = 14;         // group ID 14
    inode->atime = inode->mtime = inode->ctime = (uint64_t)now;
    for (int i = 0; i < DIRECT_MAX; i++) inode->direct[i] = direct[i];
    inode_crc_finalize(inode);
    bitmap_set(inode_bm, (size_t)free_in);

    // Write file data to allocated blocks
    for (uint64_t i=0;i<blocks_needed;i++){
        uint32_t absb = direct[i];
        uint8_t* blk = img + (size_t)absb * BS;
        size_t remain = ((size_t)fsz > (size_t)(i*BS)) ? (size_t)fsz - (size_t)(i*BS) : 0;
        size_t tocopy = (remain > BS) ? BS : remain;
        if (tocopy) memcpy(blk, fbuf + (size_t)(i*BS), tocopy);
        if (tocopy < BS) memset(blk+tocopy, 0, BS - tocopy);
    }
    free(fbuf); // free(NULL) is safe if fsz==0

    // Fill directory entry
    dirent64_t de;
    memset(&de, 0, sizeof(de));
    de.inode_no = new_ino;
    de.type = 1; // file
    strncpy(de.name, base, sizeof(de.name)-1);
    dirent_checksum_finalize(&de);
    dent[slot] = de;

    // Update root inode (. .. + files)
    root->links += 1;
    used_entries += 1;
    root->size_bytes = (uint64_t)(used_entries * sizeof(dirent64_t));
    inode_crc_finalize(root);

    // Update superblock mtime + checksum
    sb->mtime_epoch = (uint64_t)now;
    superblock_crc_finalize(sb);

    // Write output image
    FILE* fo = fopen(outpath, "wb");
    if (!fo){ perror("open output"); free(img); return 1; }
    if (fwrite(img,1,(size_t)isz,fo)!=(size_t)isz){ perror("write output"); fclose(fo); free(img); return 1; }
    fclose(fo);
    free(img);

    fprintf(stdout, "Added '%s' as inode #%u using %llu block(s) -> wrote '%s'\n",
            base, new_ino, (unsigned long long)blocks_needed, outpath);
    return 0;
}

