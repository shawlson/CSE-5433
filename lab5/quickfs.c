#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/namei.h>
#include <linux/err.h>

#include "quickfs.h"

static int quickfs_create(struct inode *, struct dentry *, int, struct nameidata *);
struct dentry *quickfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nameidata);
static int quickfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry);
static int quickfs_unlink(struct inode *dir, struct dentry *dentry);

/*
	Utility functions
*/

static int first_free_bit(struct buffer_head **buffers, unsigned long size) {

	struct buffer_head *bh;

	int block = 0;
	for (block = 0; block < size; ++block) {
		bh = buffers[block];
		unsigned char *bitmap = (unsigned char *) bh->b_data;
				
		// Search every byte in the block for a free bit
		int i;
		for (i = 0; i < QUICKFS_BLOCK_SIZE; ++i) {
			if (bitmap[i] < 0xFF) {
				if (bitmap[i] < (bitmap[i] | 0x80)) return (block * QUICKFS_BLOCK_SIZE * 8) + (i * 8) + 0;
				if (bitmap[i] < (bitmap[i] | 0x40)) return (block * QUICKFS_BLOCK_SIZE * 8) + (i * 8) + 1;
				if (bitmap[i] < (bitmap[i] | 0x20)) return (block * QUICKFS_BLOCK_SIZE * 8) + (i * 8) + 2;
				if (bitmap[i] < (bitmap[i] | 0x10)) return (block * QUICKFS_BLOCK_SIZE * 8) + (i * 8) + 3;
				if (bitmap[i] < (bitmap[i] | 0x08)) return (block * QUICKFS_BLOCK_SIZE * 8) + (i * 8) + 4;
				if (bitmap[i] < (bitmap[i] | 0x04)) return (block * QUICKFS_BLOCK_SIZE * 8) + (i * 8) + 5;
				if (bitmap[i] < (bitmap[i] | 0x02)) return (block * QUICKFS_BLOCK_SIZE * 8) + (i * 8) + 6;
				if (bitmap[i] < (bitmap[i] | 0x01)) return (block * QUICKFS_BLOCK_SIZE * 8) + (i * 8) + 7;		
			}
		}
	}
	
	// Otherwise, we couldn't find a free bit
	return -1;
}

static void clear_bitmap_bit(struct buffer_head *bh, int index) {

	unsigned char *bitmap = (unsigned char *) bh->b_data;

	int affected_byte = index / 8;
	int affected_bit = 8 - (index % 8) - 1;

	bitmap[affected_byte] &= ~(1 << affected_bit);
}

static void mark_bit(struct buffer_head *bh, int index) {

	unsigned char *bitmap = (unsigned char *) bh->b_data;

	int affected_byte = index / 8;
	int affected_bit = 8 - (index % 8) - 1;

	bitmap[affected_byte] |= (1 << affected_bit);
}

static int test_for_bit(struct buffer_head *bh, int index) {
	unsigned char byte = *(bh->b_data + (index / 8));
	return !!(byte & (0x80 >> (index % 8)));
}

static int quickfs_write_inode(struct inode *inode, int unused) {

	unsigned long inode_num = inode->i_ino;
	if (inode_num < ROOT_INODE_NUM || inode_num > 4095) {
		return -EIO;
	}

	struct buffer_head *bh = sb_bread(inode->i_sb, INODE_NUM_TO_BLOCK_NUM(inode_num));
	if (!bh) {
		return -EIO;
	}

	struct quickfs_inode *disk_inode = (struct quickfs_inode *) bh->b_data;
	if (!disk_inode) {
		return -1;
	}

	disk_inode->umode = inode->i_mode;
	disk_inode->uid = inode->i_uid;
	disk_inode->gid = inode->i_gid;
	disk_inode->data_block_count = inode->i_blocks;
	disk_inode->size = inode->i_size;
	disk_inode->hard_links = inode->i_nlink;
	disk_inode->atime = inode->i_atime;
	disk_inode->mtime = inode->i_mtime;
	disk_inode->ctime = inode->i_ctime;
	
	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

static void quickfs_delete_inode(struct inode *inode) {

	struct super_block *sb = inode->i_sb;
	struct buffer_head *super_bh = sb_bread(sb, SUPER_BLOCK_BLOCK_NUM);
	struct buffer_head *inode_bitmap_bh = sb_bread(sb, INODE_BITMAP_BLOCK_NUM);
	struct buffer_head *data_bitmap[NUM_DATA_BITMAP_BLOCKS];
	int block;
	for (block = 0; block < NUM_DATA_BITMAP_BLOCKS; ++block) {
		data_bitmap[block] = sb_bread(sb, FIRST_DATA_BITMAP_BLOCK_NUM + block);
	}
	struct buffer_head *inode_bh = sb_bread(sb, INODE_NUM_TO_BLOCK_NUM(inode->i_ino));

	struct quickfs_inode *disk_inode = (struct quickfs_inode *) inode_bh->b_data;
	unsigned short data_block_count = disk_inode->data_block_count;
	unsigned short data_blocks[data_block_count];
	memcpy(data_blocks, disk_inode->data_blocks, data_block_count * sizeof(unsigned short));
	brelse(inode_bh);

	int i;
	for (i = 0; i < data_block_count; ++i) {
		int data_block = data_blocks[i];
		int block = DATA_BIT_TO_DATA_BITMAP_BLOCK(data_block);
		int index = DATA_BIT_TO_INDEX(data_block);
		clear_bitmap_bit(data_bitmap[block], index);
		mark_buffer_dirty(data_bitmap[block]);
	}
	
	for (block = 0; block < NUM_DATA_BITMAP_BLOCKS; ++block) {
		brelse(data_bitmap[block]);
	}

	clear_bitmap_bit(inode_bitmap_bh, inode->i_ino);
	mark_buffer_dirty(inode_bitmap_bh);
	brelse(inode_bitmap_bh);

	struct quickfs_sb *disk_sb = (struct quickfs_sb *) super_bh->b_data;
	disk_sb->data_blocks_free += data_block_count;
	disk_sb->inodes_free += 1;
	mark_buffer_dirty(super_bh);
	brelse(super_bh);

	clear_inode(inode);
}


static int quickfs_readdir(struct file *file, void *dirent, filldir_t filldir) {

     struct inode * dir = file->f_dentry->d_inode;
     struct dentry * de;
 
     if (file->f_pos) {
         return 1;
     }

	de = file->f_dentry;
	
	if (filldir(dirent, ".", 1, file->f_pos++, 4096, DT_REG)) return 1;
	if (filldir(dirent, "..", 2, file->f_pos++, 4097, DT_REG)) {
		return 1;
	}

	int ino = 1;
	struct buffer_head *inode_bitmap_bh = sb_bread(dir->i_sb, INODE_BITMAP_BLOCK_NUM);
	do {
		if (test_for_bit(inode_bitmap_bh, ino)){
			struct buffer_head *disk_inode_bh = sb_bread(dir->i_sb, INODE_NUM_TO_BLOCK_NUM(ino));	
			struct quickfs_inode *disk_inode = (struct quickfs_inode *) disk_inode_bh->b_data;
			int j;

			char *name  = disk_inode->name;
			int size = strnlen(name, MAX_NAME_LENGTH);
			if (strcmp(name, "") == 0) {
				brelse(disk_inode_bh);
				ino++;
				continue;
			}
			int corresponding_ino = ino;
			if (disk_inode->link > 0) {
				corresponding_ino = disk_inode->link;
			}
			if (filldir(dirent, name, size, file->f_pos++, corresponding_ino, DT_REG) < 0){
				brelse(disk_inode_bh);
				return 0;
			}
			brelse(disk_inode_bh);
		}
		ino++;
		
	} while (ino < 8*QUICKFS_BLOCK_SIZE);
	brelse(inode_bitmap_bh);
	return 1;  
}


static struct inode_operations quickfs_inode_ops = {
	.create = quickfs_create,
	.lookup = quickfs_lookup, 
	.link = quickfs_link,
	.unlink = quickfs_unlink
};

static int quickfs_get_block(struct inode * inode, sector_t block, struct buffer_head * bh_result, int create){

	struct super_block *sb = inode->i_sb;

	switch(create) {
		case 0: {
			struct buffer_head *disk_inode_bh = sb_bread(sb, INODE_NUM_TO_BLOCK_NUM(inode->i_ino));
			struct quickfs_inode *disk_inode = (struct quickfs_inode *) disk_inode_bh->b_data;
			
			if (block >= disk_inode->data_block_count) {
				brelse(disk_inode_bh);
				return 0;
			}

			short data_block = disk_inode->data_blocks[block];
			map_bh(bh_result, sb, DATA_BIT_NUM_TO_BLOCK_NUM(data_block));

			brelse(disk_inode_bh);
			return 0;
			break;
			}
		case 1: {
			struct buffer_head *disk_sb_bh = sb_bread(sb, SUPER_BLOCK_BLOCK_NUM);
			struct quickfs_sb *disk_sb = (struct quicks_sb *) disk_sb_bh->b_data;

			struct buffer_head *disk_inode_bh = sb_bread(sb, INODE_NUM_TO_BLOCK_NUM(inode->i_ino));
			struct quickfs_inode *disk_inode = (struct quickfs_inode *) disk_inode_bh->b_data;
			
			if (disk_sb->data_blocks_free ==0) {
				brelse(disk_sb_bh);
				return -ENOSPC;
			}

			if (inode->i_size && block < disk_inode->data_block_count) {
				short data_block = disk_inode->data_blocks[block];
				map_bh(bh_result, sb, DATA_BIT_NUM_TO_BLOCK_NUM(data_block));
				return 0;
			}

			int offset;
			struct buffer_head *data_bitmap[NUM_DATA_BITMAP_BLOCKS];
			for (offset = 0; offset < NUM_DATA_BITMAP_BLOCKS; ++offset) {
				data_bitmap[offset] = sb_bread(sb, FIRST_DATA_BITMAP_BLOCK_NUM + offset);
			}

			short first_free = first_free_bit(data_bitmap, NUM_DATA_BITMAP_BLOCKS);
			offset = DATA_BIT_TO_DATA_BITMAP_BLOCK(first_free);
			int index = DATA_BIT_TO_INDEX(first_free);
			mark_bit(data_bitmap[offset], index);

			disk_inode->data_blocks[inode->i_blocks] = first_free;
			disk_sb->data_blocks_free -= 1;

			mark_buffer_dirty(disk_sb_bh);
			mark_buffer_dirty(disk_inode_bh);

			map_bh(bh_result, sb, DATA_BIT_NUM_TO_BLOCK_NUM(first_free));
			inode->i_blocks += 1;
			mark_inode_dirty(inode);
			
			brelse(disk_sb_bh);
			brelse(disk_inode_bh);
			for (offset = 0; offset < NUM_DATA_BITMAP_BLOCKS; ++offset) {
				brelse(data_bitmap[offset]);
			}

			return 0;
			break;
			}
	}

	return -EIO;
}

static int quickfs_readpage(struct file *file, struct page *page){
	return block_read_full_page(page, quickfs_get_block);
}

static int quickfs_writepage(struct page *page, struct writeback_control *wbc){
	return block_write_full_page(page, quickfs_get_block, wbc);
}

static int quickfs_prepare_write(struct file *file, struct page *page, unsigned from, unsigned to){
	return block_prepare_write(page, from, to, quickfs_get_block);
}

static struct address_space_operations quickfs_addr_space_ops = {
	.readpage = quickfs_readpage,
	.writepage = quickfs_writepage,
	.sync_page = block_sync_page,
	.prepare_write = quickfs_prepare_write,
	.commit_write = generic_commit_write
};

static struct file_operations quickfs_file_ops = {
	.llseek = generic_file_llseek,
	.read = generic_file_read,
	.write = generic_file_write,
	.mmap = generic_file_mmap,
	.sendfile = generic_file_sendfile,
	.fsync = file_fsync
};

static int quickfs_create(struct inode *inode, struct dentry *dentry, int mode, struct nameidata *nameidata) {
	
	int retval = 0;

	// Check for free inode on disk
	struct super_block *sb = inode->i_sb;
	struct buffer_head *inode_bm_bh = sb_bread(sb, INODE_BITMAP_BLOCK_NUM);
	int free_inode_num = first_free_bit(&inode_bm_bh, NUM_INODE_BITMAP_BLOCKS);
	if (free_inode_num < 0) {return -ENOSPC;}

	// Allocate new in-memory inode
	struct inode *created_inode = new_inode(sb);
	if (!created_inode)	return -ENOMEM;

	// Populate new in-memory inode
	created_inode->i_ino = free_inode_num;
	created_inode->i_mode = mode;
	created_inode->i_blksize = QUICKFS_BLOCK_SIZE;
	created_inode->i_sb = sb;
	created_inode->i_uid = current->fsuid;
	created_inode->i_gid = current->fsgid;
	created_inode->i_atime = created_inode->i_mtime = created_inode->i_ctime = CURRENT_TIME;
	created_inode->i_blkbits = QUICKFS_BLOCK_SIZE_BITS;
	created_inode->i_op = &quickfs_inode_ops;
	created_inode->i_fop = &quickfs_file_ops;
	created_inode->i_mapping->a_ops = &quickfs_addr_space_ops;
	created_inode->i_mode |= S_IFREG;

	// Write new quickfs_inode to disk
	struct buffer_head *disk_inode_bh = sb_bread(sb, INODE_NUM_TO_BLOCK_NUM(free_inode_num));
	struct quickfs_inode *disk_inode = (struct quickfs_inode *) disk_inode_bh->b_data;
	strcpy(disk_inode->name, dentry->d_name.name);
	disk_inode->size = 0;
	disk_inode->data_block_count = 0;
	disk_inode->hard_links = 1;
	disk_inode->link = -1;
	disk_inode->uid = created_inode->i_uid;
	disk_inode->gid = created_inode->i_gid;
	disk_inode->umode = created_inode->i_mode;
	disk_inode->atime = disk_inode->mtime = disk_inode->ctime = created_inode->i_ctime;
	mark_buffer_dirty(disk_inode_bh);
	brelse(disk_inode_bh);

	// Modify on-disk superblock
	struct buffer_head *quickfs_sb_bh = sb_bread(sb, SUPER_BLOCK_BLOCK_NUM);
	struct quickfs_sb *disk_sb = (struct quickfs_sb *) quickfs_sb_bh->b_data;
	disk_sb->inodes_free--;
	mark_buffer_dirty(quickfs_sb_bh);
	brelse(quickfs_sb_bh);

	// Update free inode count in quickfs_sb
	mark_bit(inode_bm_bh, free_inode_num);
	mark_buffer_dirty(inode_bm_bh);
	brelse(inode_bm_bh);

	// Mark the inode we created as dirty
	insert_inode_hash(created_inode);
	mark_inode_dirty(created_inode);

	// Instantiate dentry
	d_instantiate(dentry, created_inode);	

	return retval;
}

struct dentry *quickfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nameidata) {

	struct inode * inode = NULL;

	if (dentry->d_name.len > MAX_NAME_LENGTH) return ERR_PTR(-ENAMETOOLONG);

	int i;
	struct buffer_head *inode_bitmap_bh = sb_bread(dir->i_sb, INODE_BITMAP_BLOCK_NUM);
	for (i = 0; i < 4096; ++i) {
		if (test_for_bit(inode_bitmap_bh, i)) {
			struct buffer_head *inode_bh = sb_bread(dir->i_sb, INODE_NUM_TO_BLOCK_NUM(i));
			struct quickfs_inode *disk_inode = (struct quickfs_inode *) inode_bh->b_data;
			if (strcmp(dentry->d_iname, disk_inode->name) == 0) {
				int ino = i;
				if (disk_inode->link > 0) {
					ino = disk_inode->link;
				}
				brelse(inode_bh);
				inode = iget(dir->i_sb, ino);
				if (!inode) {return ERR_PTR(-EACCES);}
				d_add(dentry, inode);
				goto out;
			}
			brelse(inode_bh);
		}
	}

out:
	brelse(inode_bitmap_bh);
	return NULL;
}

static int quickfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry) {

	/*
		Basic process:
		Get referrenced_inode from old_dentry
		We increase the number of hard_links on referrenced_inode
		mark_referrenced_inode as dirty
		d_instantiate new_dentry with referrenced_inode

		We also:
		Go to disk and find a free disk inode
		Write new_dentry.name to disk inode
		Write referrenced_inode.number to disk_inode.link
		Modify on disk superblock
	*/
	struct inode * referrenced_inode = old_dentry->d_inode;
	struct super_block *sb = referrenced_inode->i_sb;

	// Check for free inode in bitmap
	struct buffer_head *inode_bm_bh = sb_bread(sb, INODE_BITMAP_BLOCK_NUM);
	int free_disk_inode_num = first_free_bit(&inode_bm_bh, NUM_INODE_BITMAP_BLOCKS);
	if (free_disk_inode_num < 0) {
		brelse(inode_bm_bh);
		return -ENOSPC;
	}

	// Get free inode from disk
	struct buffer_head *disk_inode_bh = sb_bread(sb, INODE_NUM_TO_BLOCK_NUM(free_disk_inode_num));
	struct quickfs_inode *disk_inode = (struct quickfs_inode *) disk_inode_bh->b_data;

	// Write appropriate fields to inode
	strcpy(disk_inode->name, new_dentry->d_name.name);
	disk_inode->link = referrenced_inode->i_ino;	
	mark_buffer_dirty(disk_inode_bh);
	brelse(disk_inode_bh);

	// Mark disk_inode as used
	mark_bit(inode_bm_bh, free_disk_inode_num);
	mark_buffer_dirty(inode_bm_bh);
	brelse(inode_bm_bh);

	// Alter on-disk superblock to reflect decrease in inodes
	struct buffer_head *superblock_bh = sb_bread(sb, SUPER_BLOCK_BLOCK_NUM);
	struct quickfs_sb *disk_sb = (struct quickfs_sb *) superblock_bh->b_data;
	disk_sb->inodes_free--;
	mark_buffer_dirty(superblock_bh);
	brelse(superblock_bh);	

	// Modify referrenced inode appropriately
	referrenced_inode->i_nlink++;
	referrenced_inode->i_ctime = CURRENT_TIME;
	referrenced_inode->i_atime = referrenced_inode->i_ctime;
	mark_inode_dirty(referrenced_inode);
	atomic_inc(&referrenced_inode->i_count);
	d_instantiate(new_dentry, referrenced_inode);
	return 0;
}

static int quickfs_unlink(struct inode *dir, struct dentry *dentry) {

	/**
	 * All unlink possibilities
	 *
	 * I. The referenced inode has one hard link
	 *	
	 *    a. The dentry's name matches the name stored in the disk
	 *       inode with the same ino:
	 *      - We only need to decrease the reference count, and 
	 *      - VFS will take care of the rest
	 *
	 *    b. The dentry's name doesn't match the name stored in
	 *       the disk inode with the same ino:
	 *      - We find the disk inode with the same name as that
	 *      - stored in the dentry. We must clear this inode from the
	 *      - inode bitmap and update the superblock appropriately. Then
	 *      - we can decrease the reference count of the inode passed in,
	 *      - and VFS will take care of the rest
	 * 
	 * II. The referenced inode has more than one hard link
	 *
	 *    a. The dentry's name matches the name stored in the
	 *       disk inode with the same ino:
	 *      - We set the disk inode's name to the empty string and
	 *      - decrease the reference count
	 *
	 *    b. The dentry's name doesn't match the name stored in
	 *       the disk inode with the same ino:
	 *      - We find the disk inode whose name matches the one that
	 *      - is passed in, clear it from the bitmap, update the super
	 *      - block appropriately, and decrease the reference count of the
	 *      - inode that was passed in
	 */

	struct super_block *sb = dir->i_sb;
	struct inode *inode = dentry->d_inode;
	const char *name = dentry->d_name.name;	

	struct buffer_head *bh = sb_bread(sb, INODE_NUM_TO_BLOCK_NUM(inode->i_ino));
	struct quickfs_inode *disk_inode = (struct quickfs_inode *) bh->b_data;

	int i;
	struct buffer_head *inode_bitmap_bh;

	struct buffer_head *disk_sb_bh;
	struct quickfs_sb *disk_sb;

	// If there's only one hard link
	if (inode->i_nlink == 1) {
		// And the filename matches the filename stored on disk
		if (strcmp(name, disk_inode->name) == 0) {goto decrease;}

		// And the filename doesn't match the filename stored on disk
		else {
			brelse(bh);
			inode_bitmap_bh = sb_bread(sb, INODE_BITMAP_BLOCK_NUM);
			for (i = 0; i < 4096; ++i) {
				if (test_for_bit(inode_bitmap_bh, i)) {
					bh = sb_bread(sb, INODE_NUM_TO_BLOCK_NUM(i));
					disk_inode = (struct quickfs_inode *) bh->b_data;
					if (strcmp(name, disk_inode->name) == 0 &&
						disk_inode->link == inode->i_ino)
					{
						goto modify_disk;
					}
					else {
						brelse(bh);
					}
				}
			}
			brelse(inode_bitmap_bh);
			goto out_error;
		}
	}

	// If there's more than one hard link
	else {
		// And the filename matches the filename stored on disk
		if (strcmp(name, disk_inode->name) == 0) {
			disk_inode->name[0] = '\0';
			mark_buffer_dirty(bh);
			goto decrease;
		}
		// And the filename doesn't match the filename stored on disk
		else {
			brelse(bh);
			inode_bitmap_bh = sb_bread(sb, INODE_BITMAP_BLOCK_NUM);
			for (i = 0; i < 4096; ++i) {
				if (test_for_bit(inode_bitmap_bh, i)) {
					bh = sb_bread(sb, INODE_NUM_TO_BLOCK_NUM(i));
					disk_inode = (struct quickfs_inode *) bh->b_data;
					if (strcmp(name, disk_inode->name) == 0 &&
						disk_inode->link == inode->i_ino)
					{
						goto modify_disk;
					}
					else {
						brelse(bh);
					} 
				}
			}
			brelse(inode_bitmap_bh);
			goto out_error;
		}

	}

modify_disk:
	disk_sb_bh = sb_bread(sb, SUPER_BLOCK_BLOCK_NUM);
	disk_sb = (struct quickfs_sb *) disk_sb_bh->b_data;
	disk_sb->inodes_free++;
	clear_bitmap_bit(inode_bitmap_bh, i);
	mark_buffer_dirty(disk_sb_bh);
	brelse(disk_sb_bh);
	mark_buffer_dirty(inode_bitmap_bh);
	brelse(disk_sb_bh);
decrease:
	brelse(bh);
	inode->i_nlink--;
	mark_inode_dirty(inode);
	
	return 0;

out_error:
	return -1;
}

static struct file_operations quickfs_dir_ops = {
	.readdir = quickfs_readdir,
	.read = generic_read_dir,
	.fsync = file_fsync
};

void quickfs_read_inode(struct inode *inode) {

	// Read from disk block into memory structure
	struct buffer_head *bh = sb_bread(inode->i_sb, INODE_NUM_TO_BLOCK_NUM(inode->i_ino));
	struct quickfs_inode *disk_inode = (struct quickfs_inode *) bh->b_data;

	// Fill in VFS inode
	inode->i_mode = disk_inode->umode;
	inode->i_uid = disk_inode->uid;
	inode->i_gid = disk_inode->gid;
	inode->i_atime = disk_inode->atime;
	inode->i_mtime = disk_inode->mtime;
	inode->i_ctime = disk_inode->ctime;
	inode->i_blocks = disk_inode->data_block_count;
	inode->i_size = disk_inode->size;
	inode->i_bytes = disk_inode->size % QUICKFS_BLOCK_SIZE;
	inode->i_blksize = QUICKFS_BLOCK_SIZE;
	inode->i_blkbits = QUICKFS_BLOCK_SIZE_BITS;
	inode->i_nlink = disk_inode->hard_links;
	if (inode->i_ino == ROOT_INODE_NUM) {
		inode->i_mode |= S_IFDIR;
		inode->i_fop = &quickfs_dir_ops;
		inode->i_op = &quickfs_inode_ops;
	}
	else{
		inode->i_mode |= S_IFREG;
		inode->i_fop = &quickfs_file_ops;
		inode->i_op = &quickfs_inode_ops;
		inode->i_mapping->a_ops = &quickfs_addr_space_ops;
	}

	brelse(bh);
}

static struct super_operations quickfs_sb_ops = {
	.read_inode = quickfs_read_inode,
	.write_inode = quickfs_write_inode,
	.delete_inode = quickfs_delete_inode
};

int quickfs_fill_super(struct super_block *sb, void *data, int silent) {
	
	// Read info from disk and create in-memory quickfs_sb
	struct buffer_head *bh = sb_bread(sb, ROOT_INODE_NUM);
	struct quickfs_sb *quickfs_disk_sb, *quickfs_info;
	quickfs_disk_sb = (struct quickfs_sb *) bh->b_data;
	quickfs_info = kmalloc(sizeof(struct quickfs_sb), GFP_KERNEL);
	quickfs_info->magic_number = quickfs_disk_sb->magic_number;
	quickfs_info->data_blocks_free = quickfs_disk_sb->data_blocks_free;
	quickfs_info->inodes_free = quickfs_disk_sb->inodes_free;
	brelse(bh);	
	
	// Fill in VFS superblock
	sb_set_blocksize(sb, QUICKFS_BLOCK_SIZE);
	sb->s_fs_info = quickfs_info;
	sb->s_magic = MAGIC_NUMBER;
	sb->s_blocksize = QUICKFS_BLOCK_SIZE;
	sb->s_blocksize_bits = QUICKFS_BLOCK_SIZE_BITS;
	sb->s_maxbytes = QUICKFS_BLOCK_SIZE * MAX_DATA_BLOCKS_PER_INODE;

	sb->s_op = &quickfs_sb_ops;

	// Allocate a root inode
	struct inode *root_inode = iget(sb, ROOT_INODE_NUM);
	sb->s_root = d_alloc_root(root_inode);
	
	return 0;
}

static struct super_block *quickfs_get_sb(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
	{
     		return get_sb_bdev(fs_type, flags, dev_name, data, quickfs_fill_super);
	}


static struct file_system_type quickfs = {
	.name = "quickfs",
	.fs_flags = FS_REQUIRES_DEV,
	.get_sb = quickfs_get_sb,
	.kill_sb = kill_block_super,
	.owner = THIS_MODULE
};

static int __init quickfs_init(void) {
	return register_filesystem(&quickfs);
}

static void __exit quickfs_exit(void) {
	unregister_filesystem(&quickfs);
}

module_init(quickfs_init);
module_exit(quickfs_exit);
	
