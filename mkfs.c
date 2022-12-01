/*
 * This code is provided solely for the personal and private use of students
 * taking the CSC369H course at the University of Toronto. Copying for purposes
 * other than this use is expressly prohibited. All forms of distribution of
 * this code, including but not limited to public repositories on GitHub,
 * GitLab, Bitbucket, or any other online platform, whether as given or with
 * any changes, are expressly prohibited.
 *
 * Authors: Alexey Khrabrov, Karen Reid
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2019 Karen Reid
 */

/**
 * CSC369 Assignment 1 - a1fs formatting tool.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#include "a1fs.h"
#include "map.h"

/*Helper function*/
int ceiling(size_t n_inodes){
	unsigned int k = n_inodes / 32;
	if (k * 32 < n_inodes){
		return k + 1;
	}else{
		return k;
	}
}

int ceiling_bit(uint64_t number){
	unsigned int k = number / 8;
	if (k * 8 < number){
		return k + 1;
	}else{
		return k;
	}
}

/** Command line options. */
typedef struct mkfs_opts {
	/** File system image file path. */
	const char *img_path;
	/** Number of inodes. */
	size_t n_inodes;
	/** Print help and exit. */
	bool help;
	/** Overwrite existing file system. */
	bool force;
	/** Sync memory-mapped image file contents to disk. */
	bool sync;
	/** Verbose output. If false, the program must only print errors. */
	bool verbose;
	/** Zero out image contents. */
	bool zero;

} mkfs_opts;

static const char *help_str = "\
Usage: %s options image\n\
\n\
Format the image file into a1fs file system. The file must exist and\n\
its size must be a multiple of a1fs block size - %zu bytes.\n\
\n\
Options:\n\
    -i num  number of inodes; required argument\n\
    -h      print help and exit\n\
    -f      force format - overwrite existing a1fs file system\n\
    -s      sync image file contents to disk\n\
    -v      verbose output\n\
    -z      zero out image contents\n\
";

static void print_help(FILE *f, const char *progname)
{
	fprintf(f, help_str, progname, A1FS_BLOCK_SIZE);
}


static bool parse_args(int argc, char *argv[], mkfs_opts *opts)
{
	char o;
	while ((o = getopt(argc, argv, "i:hfsvz")) != -1) {
		switch (o) {
			case 'i': opts->n_inodes = strtoul(optarg, NULL, 10); break;

			case 'h': opts->help    = true; return true;// skip other arguments
			case 'f': opts->force   = true; break;
			case 's': opts->sync    = true; break;
			case 'v': opts->verbose = true; break;
			case 'z': opts->zero    = true; break;

			case '?': return false;
			default : assert(false);
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Missing image path\n");
		return false;
	}
	opts->img_path = argv[optind];

	if (opts->n_inodes == 0) {
		fprintf(stderr, "Missing or invalid number of inodes\n");
		return false;
	}
	return true;
}


/** Determine if the image has already been formatted into a1fs. */
static bool a1fs_is_present(void *image)
{
	//TODO
    struct a1fs_superblock *sb = (struct a1fs_superblock *)(image);
	if (sb->magic != A1FS_MAGIC) {
		return false;
	}
	return true;
}


/**
 * Format the image into a1fs.
 *
 * NOTE: Must update mtime of the root directory.
 *
 * @param image  pointer to the start of the image.
 * @param size   image size in bytes.
 * @param opts   command line options.
 * @return       true on success;
 *               false on error, e.g. options are invalid for given image size.
 */
static bool mkfs(void *image, size_t size, mkfs_opts *opts)
{
	//TODO 
	// (void)image; // A result from mmap(NULL, 4096 * n, ...)
	// (void)size; // 4096 * n
	// (void)opts; // Maybe "i" is the only useful here since others are coverd
	
	/** The total number of blocks that can be partitioned in this file system */
	size_t max_segs = size / A1FS_BLOCK_SIZE;
	
	/** Input number of inodes is too large to fit in the file system */
	if (opts->n_inodes / 32 > max_segs) { return false; }

    /** Initialize superblock */
    struct a1fs_superblock *sb = (struct a1fs_superblock *)(image); // First block
	sb->size = size;
	sb->magic = A1FS_MAGIC;
	sb->inode_count = opts->n_inodes; // Get from input
	sb->inode_blocks = ceiling(opts->n_inodes); // Number of inodes != Number of blocks they will occupy
	sb->free_inodes_count = opts->n_inodes - 1; // One inode is reserved for root directory
	sb->data_block_count = max_segs - 4 - sb->inode_blocks;
	sb->free_data_block_count = max_segs - 5 - sb->inode_blocks;
	sb->reserved_extent_number = 1; // The first reserved extent will be "0"

	/** Set the inode bitmap; */
    unsigned char *inode_bitmap = (unsigned char *)(image + A1FS_BLOCK_SIZE * 1); // Second block
	
	for (int i = 0; i < ceiling_bit(sb->inode_count + 1); i++) {
		inode_bitmap[i] = inode_bitmap[i] & 0;	
	}
	inode_bitmap[0] |= (1 << 0); // The first inode is reserved
	inode_bitmap[0] |= (1 << 1);
	sb->inode_bitmap = inode_bitmap; // Add it to the superblock

	/** Set the block bitmap; */
    unsigned char *block_bitmap = (unsigned char *)(image + A1FS_BLOCK_SIZE * 2); // Third block
	
	for (int i = 0; i < ceiling_bit(sb->data_block_count + 1); i++) {
		block_bitmap[i] = block_bitmap[i] & 0;	
	}
	block_bitmap[0] |= (1 << 0); // The first block is reserved
	block_bitmap[0] |= (1 << 1);
	sb->block_bitmap = block_bitmap; // Add it to the superblock

	/** Set the extent block; */
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(image + A1FS_BLOCK_SIZE * 3); // Forth block
	for (int extent_number = 1;extent_number < 512;extent_number++){
		struct a1fs_extent new_extent;
		new_extent.start = 0; 
		new_extent.count = 0;
		memcpy(image+ A1FS_BLOCK_SIZE * 3+ extent_number * sizeof(struct a1fs_extent), &new_extent, sizeof(struct a1fs_extent));
	}
	
	/** Set the first inode (each block will contain 32 inodes) */
	struct a1fs_inode *inode = (struct a1fs_inode *)(image + A1FS_BLOCK_SIZE * 4); // Fifth block
	inode->mode = S_IFDIR | 0755;
	inode->links = 2;
	inode->size = 2 * sizeof(struct a1fs_dentry);
	struct timespec tp;
	if (clock_gettime(CLOCK_REALTIME, &tp) != 0) { return false; }
	inode->mtime = tp;
	inode->extent_number[0] = 1; // First extent of the first inode is defined
	for (int i = 1; i < 24; i++) {
		inode->extent_number[i] = 0;
	}

	
	/** Set the first data block for root directory */
	//struct a1fs_dentry *root_directory = (struct a1fs_dentry *)(image + A1FS_BLOCK_SIZE * (4 + sb->inode_blocks));

	// initialize the "." entry
	struct a1fs_dentry first_dentry;
	first_dentry.ino = 0;
	first_dentry.name[0] = '\0';
	strcat(first_dentry.name, ".");
	//root_directory[0] = first_dentry;
	memcpy((image + A1FS_BLOCK_SIZE * (4 + sb->inode_blocks)), &first_dentry, sizeof(struct a1fs_dentry));
	
	// initialize the ".." entry
	struct a1fs_dentry second_dentry;
	second_dentry.ino = 0;
	second_dentry.name[0] = '\0';
	strcat(second_dentry.name, "..");
	//root_directory[1] = second_dentry;
	memcpy(image + A1FS_BLOCK_SIZE * (4 + sb->inode_blocks) + sizeof(struct a1fs_dentry), &second_dentry, sizeof(struct a1fs_dentry));

	// initialize other entries as empty ones
	for (unsigned int i = 2; i < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); i++) {
		struct a1fs_dentry empty_dentry;
		empty_dentry.name[0] = '\0';
		empty_dentry.ino = opts->n_inodes + 1;
		//root_directory[i] = empty_dentry;
		memcpy(image + A1FS_BLOCK_SIZE * (4 + sb->inode_blocks) + i * sizeof(struct a1fs_dentry), &empty_dentry, sizeof(struct a1fs_dentry));
	}
	/** The extent for root directory */
	struct a1fs_extent root_extent;
	root_extent.start = 4 + sb->inode_blocks;
	root_extent.count = 1;
	extent_block[0] = root_extent;
	
	/** Do memcpy */
	memcpy(image, sb, sizeof(struct a1fs_superblock));
	memcpy(image + A1FS_BLOCK_SIZE * 3, &root_extent, sizeof(struct a1fs_extent));
	memcpy(image + A1FS_BLOCK_SIZE * 4, inode, sizeof(struct a1fs_inode));
	
	return true;
}

int main(int argc, char *argv[])
{
	mkfs_opts opts = {0};// defaults are all 0
	if (!parse_args(argc, argv, &opts)) {
		// Invalid arguments, print help to stderr
		print_help(stderr, argv[0]);
		return 1;
	}

	if (opts.help) {
		// Help requested, print it to stdout
		print_help(stdout, argv[0]);
		return 0;
	}

	// Map image file into memory
	size_t size;
	void *image = map_file(opts.img_path, A1FS_BLOCK_SIZE, &size);
	if (image == NULL) return 1;

	// Check if overwriting existing file system
	int ret = 1;
	if (!opts.force && a1fs_is_present(image)) {
		fprintf(stderr, "Image already contains a1fs; use -f to overwrite\n");
		goto end;
	}
	

	if (opts.zero) memset(image, 0, size);
	if (!mkfs(image, size, &opts)) {
		fprintf(stderr, "Failed to format the image\n");
		goto end;
	}


	// Sync to disk if requested
	if (opts.sync && (msync(image, size, MS_SYNC) < 0)) {
		perror("msync");
		goto end;
	}


	ret = 0;
end:
	munmap(image, size);
	return ret;
}
