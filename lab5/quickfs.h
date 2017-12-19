#ifndef QUICKFS_HEADER
#define QUICKFS_HEADER

#include <linux/types.h>
#include <linux/fs.h>

#define QUICKFS_BLOCK_SIZE 512
#define QUICKFS_BLOCK_SIZE_BITS 9
#define MAX_NUMBER_INODES (QUICKFS_BLOCK_SIZE * 8)
#define NUM_DATA_BITMAP_BLOCKS 4
#define NUM_INODE_BITMAP_BLOCKS 1
#define MAX_NUMBER_DATA_BLOCKS (QUICKFS_BLOCK_SIZE * NUMBER_DATA_BITMAP_BLOCKS * 8)

#define SUPER_BLOCK_BLOCK_NUM 0
#define INODE_BITMAP_BLOCK_NUM 1
#define FIRST_DATA_BITMAP_BLOCK_NUM 2
#define FIRST_INODE_BLOCK_NUM 6
#define FIRST_DATA_BLOCK_NUM 4102
#define MAGIC_NUMBER 0xFEEDD0BB

struct quickfs_sb {
	unsigned long magic_number;
	unsigned long data_blocks_free;
	unsigned long inodes_free;
};

#define ROOT_INODE_NUM 0
#define INODE_NUM_TO_BLOCK_NUM(NUM) (FIRST_INODE_BLOCK_NUM + NUM)
#define DATA_BIT_NUM_TO_BLOCK_NUM(NUM) (FIRST_DATA_BLOCK_NUM + NUM)
#define DATA_BIT_TO_DATA_BITMAP_BLOCK(INDEX) (INDEX / (8 * QUICKFS_BLOCK_SIZE))
#define DATA_BIT_TO_INDEX(INDEX) (INDEX % (8 * QUICKFS_BLOCK_SIZE))
#define MAX_NAME_LENGTH 256
#define MAX_DATA_BLOCKS_PER_INODE 104
struct quickfs_inode {

	char name[MAX_NAME_LENGTH];
	unsigned short size;
	
	unsigned short data_block_count;
	unsigned short data_blocks[MAX_DATA_BLOCKS_PER_INODE];

	unsigned long hard_links;
	short link;

	uid_t uid;
	gid_t gid;
	umode_t umode;
	
	struct timespec atime;
	struct timespec mtime;
	struct timespec ctime;

};

#endif

