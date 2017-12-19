#include <linux/fs.h>
#include <sys/types.h>
#include <unistd.h>
#include "quickfs.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define SUPER_BLOCK_POS (SUPER_BLOCK_BLOCK_NUM * QUICKFS_BLOCK_SIZE)
#define INODE_BITMAP_POS (INODE_BITMAP_BLOCK_NUM * QUICKFS_BLOCK_SIZE)
#define DATA_BITMAP_POS (FIRST_DATA_BITMAP_BLOCK_NUM * QUICKFS_BLOCK_SIZE)
#define INODES_POS (FIRST_INODE_BLOCK_NUM * QUICKFS_BLOCK_SIZE)
#define DATA_POS (FIRST_DATA_BLOCK_NUM * QUICKFS_BLOCK_SIZE)

inline int bytes_to_data_blocks(unsigned long bytes) {
	return (((int) bytes) - (4102 * QUICKFS_BLOCK_SIZE)) / QUICKFS_BLOCK_SIZE;
}

int write_superblock(FILE *file, unsigned long size) {
	
	int ret = 0;

	unsigned long data_blocks_free = bytes_to_data_blocks(size);
	if (data_blocks_free > NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE * 8) {
		data_blocks_free = NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE * 8;
	}
	struct quickfs_sb sb = {
		.magic_number = MAGIC_NUMBER,
		.data_blocks_free = data_blocks_free,
		.inodes_free = (QUICKFS_BLOCK_SIZE * 8) - 1 // One inode reserved for root
	};

	if (ret = fseek(file, SUPER_BLOCK_POS, SEEK_SET)) goto out;
	if (fwrite(&sb, sizeof(struct quickfs_sb), 1, file) != 1) {
		ret = -1;
		goto out;
	}
out:
	return ret;
}

int write_inode_bitmap(FILE *file) {

	int ret = 0;
	if (ret = fseek(file, INODE_BITMAP_POS, SEEK_SET)) goto out;

	unsigned char bit_map[QUICKFS_BLOCK_SIZE];
	
	// We will start with one inode for the root
	unsigned char root_reserved = 0x80;
	bit_map[0] = root_reserved;

	// The rest of the bitmap will be empty
	int i;
	unsigned char empty_bits = 0x00;
	for (i = 1; i < QUICKFS_BLOCK_SIZE; ++i) {
		bit_map[i] = empty_bits;
	}

	if (fwrite(bit_map, sizeof(unsigned char), QUICKFS_BLOCK_SIZE, file) != QUICKFS_BLOCK_SIZE) {
		ret = -1;
		goto out;
	}

out:
	return ret;		
}

int write_data_bitmap(FILE *file, unsigned long size) {

	int ret = 0;
	if (ret = fseek(file, DATA_BITMAP_POS, SEEK_SET)) goto out;

	/*
	 * Start by assuming our file is big enough to
         * allocate NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE * 8 data blocks
	 */
	unsigned char bit_map[NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE];
	unsigned char empty_bits = 0x00;
	int i;
	for (i = 0; i < NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE; ++i) {
		bit_map[i] = empty_bits;
	}
	
	/*
	 * If we have room in the file to allocate QUICKFS_BLOCK_SIZE * 8
	 * data blocks, then we're good to write the bitmap to the file
	 */
	if (bytes_to_data_blocks(size) >= NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE * 8) {
		goto write;
	}

	/*
	 * Otherwise, we have to mark some amount of the last bits in the bitmap
	 * as occupied, since our file isn't big enough to hold NUM_DATA_BITMAP_BLOCKS *
	 * QUICKFS_BLOCK_SIZE * 8 data blocks
	 *
	 * The first byte in bit_map to have a set bit may have anywhere between
	 * 1 and 8 set bits. Every byte following it will have 8 set bits.
	 */
	int first_set_bit = bytes_to_data_blocks(size);
	int first_byte_with_set_bit = first_set_bit / 8;
	
	// First byte in bit_map with a set bit
	unsigned char first_set_byte = 0x00;
	unsigned short bit_mask;
	int shifts = 8 - (first_set_bit % 8);
	for (bit_mask = 0x01; bit_mask < (0x01 << shifts); bit_mask <<= 1) {
		first_set_byte |= bit_mask;
	}
	bit_map[first_byte_with_set_bit] = first_set_byte;

	// All following bytes in the bit_map are completely occupied
	unsigned char full_bits = 0xFF;
	for (i = first_byte_with_set_bit + 1; i < NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE; ++i) {
		bit_map[i] = full_bits;
	}

write:
	if (fwrite(bit_map, sizeof(unsigned char), NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE, file) \
		!= NUM_DATA_BITMAP_BLOCKS * QUICKFS_BLOCK_SIZE)
	{
		ret = -1;
		goto out;
	}
	
out:
	return ret;
}

int write_root_inode(FILE *file) {

	int ret = 0;	

	// Write fields of inode
	struct quickfs_inode inode;
	strcpy(inode.name, ".");
	inode.size = 0;
	inode.data_block_count = 0;
	inode.hard_links = 1;
	inode.link = -1;
	inode.uid = getuid();
	inode.gid = getgid();
	inode.umode = S_IFDIR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
	clock_gettime(CLOCK_REALTIME, &inode.ctime);
	inode.atime = inode.mtime = inode.ctime;

	// Write inode to disk
	if(ret = fseek(file, INODES_POS, SEEK_SET)) goto out;
	if (fwrite(&inode, sizeof(struct quickfs_inode), 1, file) != 1) {
		ret = -1;
		goto out;
	}

out:
	return ret;
}

int main(int argc, char *argv[]) {

	if (argc != 2){
		fprintf(stderr, "usage: mkquickfs.h image\n");
		goto out_error;
	}

	// Open file
	FILE *file = fopen(argv[1], "r+");
	if (file == NULL) {
		fprintf(stderr, "Couldn't open file\n");
		goto out_error;
	}

	// Determine if file is big enough for file system
	struct stat st;
	stat(argv[1], &st);
	unsigned long size = st.st_size;
	if(bytes_to_data_blocks(size) < 1) {
		fprintf(stderr, "File not sufficient size\n");
		goto out_error;
	}
	printf("File has space for data blocks: %d\n", bytes_to_data_blocks(size));

	// Write superblock
	if (write_superblock(file, size)) goto out_error;
	printf("Superblock written\n");

	// Write inode bitmap
	if (write_inode_bitmap(file)) goto out_error;
	printf("inode bitmap written\n");

	// Write data bitmap
	if (write_data_bitmap(file, size)) goto out_error;
	printf("data bitmap written\n");

	// Write root inode
	if (write_root_inode(file)) goto out_error;
	printf("root inode written\n");

	fclose(file);

	printf("./mkquickfs: created quickfs filesystem on '%s'\n", argv[1]);
	return 0;

out_error:
	fprintf(stderr, "Image could not be formatted for quickfs\n");
	return -1;
}
	
