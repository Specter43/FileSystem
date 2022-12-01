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
 * CSC369 Assignment 1 - a1fs driver implementation.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <dirent.h>
#include <time.h>

// Using 2.9.x FUSE API
#define FUSE_USE_VERSION 29
#include <fuse.h>

#include "a1fs.h"
#include "fs_ctx.h"
#include "options.h"
#include "map.h"

//NOTE: All path arguments are absolute paths within the a1fs file system and
// start with a '/' that corresponds to the a1fs root directory.
//
// For example, if a1fs is mounted at "~/my_csc369_repo/a1b/mnt/", the path to a
// file at "~/my_csc369_repo/a1b/mnt/dir/file" (as seen by the OS) will be
// passed to FUSE callbacks as "/dir/file".
//
// Paths to directories (except for the root directory - "/") do not end in a
// trailing '/'. For example, "~/my_csc369_repo/a1b/mnt/dir/" will be passed to
// FUSE callbacks as "/dir".


/*============================================================================Helper Functions Below=============================================================*/



/** Ceiling function for ceil(number/8) 
	Helper for bitmap modification
*/
int ceiling_bit(uint64_t number) {
	unsigned int k = number / 8;
	if (k * 8 < number){
		return k + 1;
	}else{
		return k;
	}
}

/** Ceiling function for ceil(size/4096) 
	Helper for truncate
*/
unsigned int ceiling_block(off_t size) {
	unsigned int k = size / A1FS_BLOCK_SIZE;
	if (k * A1FS_BLOCK_SIZE < size){
		return k + 1;
	}else{
		return k;
	}
}


/** Helper for count the number of components 
	"/tmp/mnt/a/b/c" will return 5
*/
int count_components(char *path_cp) {
	int number_of_components = 0;
	for (unsigned int i = 0; i < strlen(path_cp); i++) {
		if (path_cp[i] == '/' && i != strlen(path_cp) - 1) {
			number_of_components++;
		}
	}
	return number_of_components;
}

/** Helper for getting a list of components 
	"/tmp/mnt/a/b/c" will return
	["tmp", "mnt", "a", "b", "c"]
*/
void list_of_components(char *path_cp, char **components) {
	char *component;
	int i = 0;
	component = strtok(path_cp, "/");
	while (component != NULL) {
		components[i] = component;
		component = strtok(NULL, "/");
		i++;
	}
}

/** 
	Assign information from the found inode to st in getattr 
*/
void assign_info(struct stat *st, struct a1fs_inode *inode) {
	st->st_mode = inode->mode;
	st->st_nlink = 2;
	st->st_size = inode->size;
	st->st_blocks = inode->size / 512;
	st->st_mtim = inode->mtime;
	printf("\n\n");
}


/** 
 *	Helper for getting the inode number of last component in a path
 */
int check_new(fs_ctx *fs, int number_of_components, char **components){

	/*Set up for the check*/
	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	struct a1fs_inode *first_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4);
	struct a1fs_inode *cur_inode = first_inode;
	int inode_number = 0;
	
	/*Check the different part*/
	for (int i = 0; i < number_of_components; i++) { // Iterate through all components
		int found = 0;
		if ((cur_inode->mode & S_IFDIR) != S_IFDIR && i != number_of_components - 1){
			inode_number = -ENOTDIR;
		}else if ((cur_inode->mode & S_IFDIR) == S_IFDIR){
			for (int j = 0; j < 24; j++) { // Iterate through 24 extent numbers in the inode
				if (cur_inode->extent_number[j] > 0) { // Valid extent number
				// Valid extent number ==> valid extent
				uint32_t valid_extent_start = extent_block[cur_inode->extent_number[j] - 1].start;
				// Valid extent's start ==> valid resered data block
				struct a1fs_dentry *subdirectories = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * valid_extent_start);
					for (unsigned int k = 0; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) { // Iterate through 16 dentries
						if (subdirectories[k].ino < sb->inode_count && strcmp(subdirectories[k].name, components[i]) == 0) { // Component found
							inode_number = subdirectories[k].ino;
							cur_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + subdirectories[k].ino * sizeof(struct a1fs_inode));
							j = 24;
							found = 1;
							break;
						}
					}
				}
			}
		}
		if (!found){
			inode_number = -ENOENT;
			break;
		}
	}
	return inode_number;
}


/** 
 * Helper for mkdir 
 */
int allocate_block(fs_ctx *fs){
    struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
    unsigned int *block_bitmap = fs->image + A1FS_BLOCK_SIZE * 2;
	int ceil = ceiling_bit(sb->data_block_count);
    for (int i = 0; i < ceil; i++) {
       for (unsigned j = 0; j < 8; j++){
			if (strcmp((block_bitmap[i] & (1 << j) ? "1" : "0"), "0") == 0){
				block_bitmap[i] |= (1 << j);
				sb->free_data_block_count--;
				return i * 8 + j + 4 + sb->inode_blocks;
			}
		}
    }
    return -ENOSPC;
}

/** 
 * Helper for mkdir 
 */
int allocate_inode(fs_ctx *fs){
    struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
    unsigned char *inode_bitmap = fs->image + A1FS_BLOCK_SIZE;
	int ceil = ceiling_bit(sb->inode_count);
    for (int i = 0; i < ceil; i++){
		for (unsigned j = 0; j < 8; j++){
			if (strcmp((inode_bitmap[i] & (1 << j) ? "1" : "0"), "0") == 0){
				inode_bitmap[i] |= (1 << j);
				sb->free_inodes_count--;
				return i * 8 + j ;
			}
		}
    }
    return -ENOSPC;
}

/** 
	Allocate extent for the new dir 
*/
int allocate_extent(fs_ctx *fs){
	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	for (int i = 1; i < 512; i++){
		if (extent_block[i].start == 0){
			sb->reserved_extent_number++;
			return i + 1;
		}
	}
	return -ENOSPC;
}

/** 
	Copy the new inode to the memory 
*/
void make_new_inode(fs_ctx *fs, int new_inode_number, mode_t mode, int extent_number, int symbol){
	struct a1fs_inode new_inode;
	new_inode.links = 2;
	if (symbol){
		new_inode.mode = mode | S_IFDIR;
		new_inode.size = 2 * sizeof(struct a1fs_dentry);
	}else{
		new_inode.mode = mode | S_IFREG;
		new_inode.size = 0;
	}
	new_inode.extent_number[0] = extent_number;
	for (int i = 1; i < 24; i++){
		new_inode.extent_number[i] = 0;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	new_inode.mtime = ts;
	memcpy(fs->image + (new_inode_number) * sizeof(struct a1fs_inode) + 4 * A1FS_BLOCK_SIZE, &new_inode, sizeof(struct a1fs_inode));
}

/** 
	Copy the new extent to the memory 
*/
void make_new_extent(fs_ctx *fs, int new_extent_number, int new_block_number){
	struct a1fs_extent new_extent;
	new_extent.start = new_block_number;
	new_extent.count = 1; 
	
	memcpy(fs->image + (new_extent_number - 1) * sizeof(struct a1fs_extent) + 3 * A1FS_BLOCK_SIZE, &new_extent, sizeof(struct a1fs_extent));
}

/** 
	Initialize the basic block for the new dir 
*/
void make_new_dir_block(fs_ctx *fs, int new_block_number, int new_inode_number){

	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	// initialize the '.' entry
	struct a1fs_dentry *root_directory = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * new_block_number);
	struct a1fs_dentry first_dentry;
	first_dentry.ino = new_inode_number;
	first_dentry.name[0] = '\0';
	strcat(first_dentry.name, ".");
	root_directory[0] = first_dentry;
	memcpy((fs->image + A1FS_BLOCK_SIZE * new_block_number), &first_dentry, sizeof(struct a1fs_dentry));
	
	// initialize the ".." entry
	struct a1fs_dentry second_dentry;
	second_dentry.ino = new_inode_number;
	second_dentry.name[0] = '\0';
	strcat(second_dentry.name, "..");
	root_directory[1] = second_dentry;
	memcpy(fs->image + A1FS_BLOCK_SIZE * new_block_number + sizeof(struct a1fs_dentry), &second_dentry, sizeof(struct a1fs_dentry));

	//Set the default value
	for (int i = 2; i < 16; i++){
		struct a1fs_dentry new_dentry;
		new_dentry.ino = sb->inode_count + 1;
		new_dentry.name[0] = '\0';
		memcpy(fs->image + A1FS_BLOCK_SIZE * new_block_number + sizeof(struct a1fs_dentry) * i, &new_dentry, sizeof(struct a1fs_dentry));
	}
}

/** 
	Update the parent dir's infomation 
*/
void update_parent(fs_ctx *fs, struct a1fs_inode *parent_inode, struct a1fs_dentry *dentry, uint32_t parent_inode_number){
	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	
	int success = 0;
	for (unsigned int j = 0; j < 24; j++){
		if (parent_inode->extent_number[j] != 0){
			uint32_t valid_extent_start = extent_block[parent_inode->extent_number[j] - 1].start;
			struct a1fs_dentry *parent_dentry = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * valid_extent_start);
			for (unsigned int k = 0; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) { // Iterate through 16 dentries
				if (parent_dentry[k].ino >= sb->inode_count) {//find a blank dentry and copy the data
					memcpy(fs->image + valid_extent_start * A1FS_BLOCK_SIZE + k * sizeof(a1fs_dentry), dentry, sizeof(a1fs_dentry));
					success = 1;
					break;
				}
			}
			if (success){
				break;
			}
		}
	}
	
	if (!success){//if the current dentry is full
		int new_block = allocate_block(fs);
		int new_extent = allocate_extent(fs);
		make_new_extent(fs, new_extent, new_block);
		make_new_dir_block(fs, new_block, parent_inode_number);
		memcpy(fs->image + new_block * A1FS_BLOCK_SIZE + 2 * sizeof(a1fs_dentry), dentry, sizeof(a1fs_dentry));
		for (int i = 0; i < 24; i++){
			if (parent_inode->extent_number[i] == 0){
				parent_inode->extent_number[i] = new_extent;
				break;
			}
		}
	}
	parent_inode->size += sizeof(a1fs_dentry);
}

/**  
	Make destination postion in block bitmap to 0
*/
void reset_bitmap(unsigned char*bitmap, uint32_t destination){
	unsigned int byte = destination / 8;
	unsigned int bit = destination % 8;
	bitmap[byte] = (bitmap[byte] &  ~(1 << bit))|(0 << bit);
}

/**  
	Make destination postion in block bitmap to 1
*/
void assign_bitmap(unsigned char*bitmap, uint32_t destination){
	unsigned int byte = destination / 8;
	unsigned int bit = destination % 8;
	bitmap[byte] |= (1 << bit);
}

/** 
 * Helper for truncate
 * Should modify the list of integers to map the postions of zeros appear in block bitmap
 */
void find_zero_postions(fs_ctx *fs, int *zero_pos) {
    struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
    unsigned char *block_bitmap = fs->image + A1FS_BLOCK_SIZE * 2;
	int ceil = ceiling_bit(sb->data_block_count);
	int pos = 0;
    for (int i = 0; i < ceil; i++) {
       	for (unsigned j = 0; j < 8; j++) {
			// Found one segment of consecutive empty blocks (within 1 byte)
			if (strcmp((block_bitmap[i] & (1 << j) ? "1" : "0"), "0") == 0) {
				zero_pos[pos] = 8 * i + j;
				pos++;
			}
		}
    }
}


/**=========================================================================A1FS System Call Function Below============================================================================*/

/**
 * Initialize the file system.
 *
 * Called when the file system is mounted. NOTE: we are not using the FUSE
 * init() callback since it doesn't support returning errors. This function must
 * be called explicitly before fuse_main().
 *
 * @param fs    file system context to initialize.
 * @param opts  command line options.
 * @return      true on success; false on failure.
 */
static bool a1fs_init(fs_ctx *fs, a1fs_opts *opts)
{
	// Nothing to initialize if only printing help or version
	if (opts->help || opts->version) return true;

	size_t size;
	void *image = map_file(opts->img_path, A1FS_BLOCK_SIZE, &size);
	if (!image) return false;

	return fs_ctx_init(fs, image, size, opts);
}

/**
 * Cleanup the file system.
 *
 * Called when the file system is unmounted. Must cleanup all the resources
 * created in a1fs_init().
 */
static void a1fs_destroy(void *ctx)
{
	fs_ctx *fs = (fs_ctx*)ctx;
	if (fs->image) {
		if (fs->opts->sync && (msync(fs->image, fs->size, MS_SYNC) < 0)) {
			perror("msync");
		}
		munmap(fs->image, fs->size);
		fs_ctx_destroy(fs);
	}
}

/** Get file system context. */
static fs_ctx *get_fs(void)
{
	return (fs_ctx*)fuse_get_context()->private_data;
}


/**
 * Get file system statistics.
 *
 * Implements the statvfs() system call. See "man 2 statvfs" for details.
 * The f_bfree and f_bavail fields should be set to the same value.
 * The f_ffree and f_favail fields should be set to the same value.
 * The f_fsid and f_flag fields are ignored.
 *
 * @param path  path to any file in the file system. Can be ignored.
 * @param st    pointer to the struct statvfs that receives the result.
 * @return      0 on success; -errno on error.
 */
static int a1fs_statfs(const char *path, struct statvfs *st)
{
	(void)path;// unused
	fs_ctx *fs = get_fs();

	memset(st, 0, sizeof(*st));
	st->f_bsize   = A1FS_BLOCK_SIZE;
	st->f_frsize  = A1FS_BLOCK_SIZE;
	
	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	if (sb->magic != A1FS_MAGIC){
		return -1;
	}
	// Assign information
	st->f_files = sb->inode_count;
	st->f_blocks = sb->size;
	st->f_bfree = sb->free_data_block_count;
	st->f_bavail = sb->free_data_block_count;
	st->f_ffree = sb->free_inodes_count;
	st->f_favail = sb->free_inodes_count;
	st->f_namemax = A1FS_NAME_MAX;

	return 0;
}

/**
 * Get file or directory attributes.
 *
 * Implements the stat() system call. See "man 2 stat" for details.
 * The st_dev, st_blksize, and st_ino fields are ignored.
 *
 * NOTE: the st_blocks field is measured in 512-byte units (disk sectors).
 *
 * Errors:
 *   ENAMETOOLONG  the path or one of its components is too long.
 *   ENOENT        a component of the path does not exist.
 *   ENOTDIR       a component of the path prefix is not a directory.
 *
 * @param path  path to a file or directory.
 * @param st    pointer to the struct stat that receives the result.
 * @return      0 on success; -errno on error;
 */
static int a1fs_getattr(const char *path, struct stat *st)
{
	if (strlen(path) >= A1FS_PATH_MAX) return -ENAMETOOLONG;
	fs_ctx *fs = get_fs();

	memset(st, 0, sizeof(*st));

	// Get a copy of the path
	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);
	
	// Set up the first inode for searching
	// struct a1fs_inode *first_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4);
	
	// Check
	int found = check_new(fs, number_of_components, components);
	if (found < 0) { // component not found
		return found;
	} else { // component found
		struct a1fs_inode *good = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + sizeof(a1fs_inode) * found);
		assign_info(st, good);
		return 0;
	}
	return 0;
}

/**
 * Read a directory.
 *
 * Implements the readdir() system call. Should call filler() for each directory
 * entry. See fuse.h in libfuse source code for details.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a filler() call failed).
 *
 * @param path    path to the directory.
 * @param buf     buffer that receives the result.
 * @param filler  function that needs to be called for each directory entry.
 *                Pass 0 as offset (4th argument). 3rd argument can be NULL.
 * @param offset  unused.
 * @param fi      unused.
 * @return        0 on success; -errno on error.
 */
static int a1fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
	(void)offset;// unused
	(void)fi;// unused
	
	fs_ctx *fs = get_fs();

	// Get a copy of path
	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);
	

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);

	// Access the component
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	struct a1fs_inode *first_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4);
	struct a1fs_inode *cur_inode = first_inode;
	
	// Iterate components
	int found = check_new(fs, number_of_components, components);
	if (found < 0) { // component not found
		return found;
	}
	cur_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + found * sizeof(struct a1fs_inode));
	for (int i = 0; i < 24; i++) {
		if (cur_inode->extent_number[i] > 0) { // current extent makes sense
			uint32_t valid_extent_start= extent_block[cur_inode->extent_number[i] - 1].start;
			uint32_t valid_extent_count = extent_block[cur_inode->extent_number[i] - 1].count;
			for (unsigned int j = 0; j < valid_extent_count; j++) {
				struct a1fs_dentry *cur_dentry = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * (valid_extent_start + j));
				for (unsigned int k = 0; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) {
					if (strcmp(cur_dentry[k].name, "") != 0) { // current dentry is meaningful
						if (filler(buf, cur_dentry[k].name, NULL, 0) != 0) {
							return -ENOMEM;
						}
					}
				}
			}	
		}
	}
	return 0;
}


/**
 * Create a directory.
 *
 * Implements the mkdir() system call.
 *
 * NOTE: the mode argument may not have the type specification bits set, i.e.
 * S_ISDIR(mode) can be false. To obtain the correct directory type bits use
 * "mode | S_IFDIR".
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the directory to create.
 * @param mode  file mode bits.
 * @return      0 on success; -errno on error.
 */
static int a1fs_mkdir(const char *path, mode_t mode)
{
	fs_ctx *fs = get_fs();

	/*Get a copy of path*/
	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);

	// Access the component
	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	struct a1fs_inode *first_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4);
	struct a1fs_inode *cur_inode = first_inode;
	struct a1fs_dentry *subdirectories = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * (4 + (sb->inode_blocks)));
	uint32_t valid_extent_start = extent_block[cur_inode->extent_number[0] - 1].start;
	uint32_t parent_inode_number = 0;
	
	for (int i = 0; i < number_of_components; i++) { // Iterate through all components
		if (i == number_of_components - 1){
			/*Find the name of the last component(new dir)*/
			char name[A1FS_NAME_MAX];
			name[0] = '\0';
			strncat(name, components[number_of_components - 1], strlen (components[number_of_components - 1]));
			
			/*Make new dictionary entry in parent*/
			struct a1fs_dentry new_dentry;
			new_dentry.name[0] = '\0';
			strcat(new_dentry.name, name);
			int new_inode_number = allocate_inode(fs);
			if (new_inode_number < 0) {
				return -ENOSPC;
			}
			int new_block_number = allocate_block(fs);
			if (new_block_number < 0) {
				return -ENOSPC;
			}
			int new_extent_number = allocate_extent(fs);
			if (new_extent_number < 0) {
				return -ENOSPC;
			}

			/*check if the inode is allocated*/
			new_dentry.ino = new_inode_number;
			make_new_inode(fs, new_inode_number, mode, new_extent_number, 1);
			make_new_extent(fs, new_extent_number, new_block_number);
			make_new_dir_block(fs, new_block_number, new_inode_number);
			update_parent(fs, cur_inode, &new_dentry, parent_inode_number);
			return 0;
		}

		for (int j = 0; j < 24; j++) { // Iterate through 24 extent numbers in the inode
			if (cur_inode->extent_number[j] > 0) { // Valid extent number
				// Valid extent number ==> valid extent
				valid_extent_start = extent_block[cur_inode->extent_number[j] - 1].start;
				// Valid extent's start ==> valid resered data block
				subdirectories = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * valid_extent_start);
				int found = 0;
				for (unsigned int k = 0; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) { // Iterate through 16 dentries
					if (subdirectories[k].ino < sb->inode_count && strcmp(subdirectories[k].name, components[i]) == 0) { // Component found
						found = 1;
						if (i < number_of_components - 1 ) { // pass the inode to the next component
							parent_inode_number = subdirectories[k].ino;
							cur_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + subdirectories[k].ino * sizeof(struct a1fs_inode));
						}
						break;
					}
				}
				if (found) { // component found
					break;
				}
			}
		}
	}
	return 0;
}

/**
 * Remove a directory.
 *
 * Implements the rmdir() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOTEMPTY  the directory is not empty.
 *
 * @param path  path to the directory to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_rmdir(const char *path)
{
	fs_ctx *fs = get_fs();

	/*Get a copy of path*/
	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);

	// Access the component
	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	struct a1fs_inode *first_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4);
	struct a1fs_inode *cur_inode = first_inode;
	uint32_t inode_number;
	unsigned int offset;
	uint32_t block_number;
	unsigned char *block_bitmap = fs->image  + A1FS_BLOCK_SIZE * 2;

	/*touch the last inode*/
	for (int i = 0; i < number_of_components; i++) { // Iterate through all components
		for (int j = 0; j < 24; j++) { // Iterate through 24 extent numbers in the inode
			if (cur_inode->extent_number[j] > 0) { // Valid extent number
				// Valid extent number ==> valid extent
				uint32_t valid_extent_start = extent_block[cur_inode->extent_number[j] - 1].start;
				// Valid extent's start ==> valid resered data block
				struct a1fs_dentry *subdirectories = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * valid_extent_start);
				for (unsigned int k = 2; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) { // Iterate through 16 dentries
					if (subdirectories[k].ino < sb->inode_count && strcmp(subdirectories[k].name, components[i]) == 0) { // Component found
						block_number = valid_extent_start;
						offset = k;
						inode_number = subdirectories[k].ino;
						cur_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + subdirectories[k].ino * sizeof(struct a1fs_inode));
						j = 24;
						break;
					}	
				}
			}
		}
	}

	/*check if there's nothing left*/
	for (int ii = 0; ii < 24; ii++) {
		if (cur_inode->extent_number[ii] > 0) {
			uint32_t valid_extent_start= extent_block[cur_inode->extent_number[ii] - 1].start;
			uint32_t valid_extent_count = extent_block[cur_inode->extent_number[ii] - 1].count;
			for (unsigned int jj = 0; jj < valid_extent_count; jj++) {
				struct a1fs_dentry *cur_dentry = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * (valid_extent_start + jj));
				for (unsigned int kk = 2; kk < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); kk++) {
					if (strcmp(cur_dentry[kk].name, "") != 0){
						return -ENOTEMPTY;
					}
				}
				//Reset data block
				memset(fs->image + (valid_extent_start + jj) * A1FS_BLOCK_SIZE, 0, A1FS_BLOCK_SIZE);
				reset_bitmap(block_bitmap, valid_extent_start + jj);
			}
			// Reset extent
			struct a1fs_extent killer_extent;
			killer_extent.start = 0; 
			killer_extent.count = 0; 
			memcpy(fs->image + 3 * A1FS_BLOCK_SIZE + (cur_inode->extent_number[ii] - 1) * sizeof(struct a1fs_extent), &killer_extent, sizeof(struct a1fs_extent));
			sb->reserved_extent_number--;
		}
			
	}
	/*Clear dentry in parent directory*/
	struct a1fs_dentry killer;
	killer.ino = sb->inode_count + 1; 
	killer.name[0] = '\0';
	memcpy(fs->image + A1FS_BLOCK_SIZE * block_number + offset * sizeof(a1fs_dentry), &killer, sizeof(a1fs_dentry));

	/*Clear bitmap*/
	unsigned char *inode_bitmap = fs->image + A1FS_BLOCK_SIZE;
	reset_bitmap(inode_bitmap, inode_number);

	/*Reset inode*/
	struct a1fs_inode killer_inode;
	killer_inode.links = 0; 
	killer_inode.size = 0;
	for (int kill = 0; kill < 24; kill++){
		killer_inode.extent_number[kill] = 0;
	}
	memcpy(fs->image + A1FS_BLOCK_SIZE * 4 + inode_number * sizeof(struct a1fs_inode), &killer_inode, sizeof(struct a1fs_inode));
	return 0;
}

/**
 * Create a file.
 *
 * Implements the open()/creat() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to create.
 * @param mode  file mode bits.
 * @param fi    unused.
 * @return      0 on success; -errno on error.
 */
static int a1fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)fi;// unused
	assert(S_ISREG(mode));
	fs_ctx *fs = get_fs();

	/*Get a copy of path*/
	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);


	uint32_t target_dir_inode = check_new(fs, number_of_components - 1, components);
	struct a1fs_inode *inode = (struct a1fs_inode *)(fs->image + 4 * A1FS_BLOCK_SIZE + target_dir_inode * sizeof(struct a1fs_inode));

	/*Allocate space for the new file*/
	int new_block_number = allocate_block(fs);
	if (new_block_number < 0){
		return -ENOSPC;
	}
	int new_inode_number = allocate_inode(fs);
	if (new_inode_number < 0){
		return -ENOSPC;
	}
	int new_extent_number = allocate_extent(fs);
	if (new_extent_number < 0){
		return -ENOSPC;
	}

	//Set the infomation
	struct a1fs_dentry new_dentry;
	new_dentry.name[0] = '\0';
	strncat(new_dentry.name, components[number_of_components - 1], strlen(components[number_of_components - 1]));
	new_dentry.ino = new_inode_number;
	make_new_inode(fs, new_inode_number, mode, new_extent_number, 0);
	make_new_extent(fs, new_extent_number, new_block_number);
	update_parent(fs, inode, &new_dentry, target_dir_inode);
	return 0;
}

/**
 * Remove a file.
 *
 * Implements the unlink() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * @param path  path to the file to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_unlink(const char *path)
{
	fs_ctx *fs = get_fs();

	/*Get a copy of path*/
	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);

	// Access the component
	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	struct a1fs_inode *first_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4);
	struct a1fs_inode *cur_inode = first_inode;
	unsigned int offset;
	uint32_t block_number;

	/*touch the last inode*/
	for (int i = 0; i < number_of_components - 1; i++) { // Iterate through all components
		for (int j = 0; j < 24; j++) { // Iterate through 24 extent numbers in the inode
			if (cur_inode->extent_number[j] > 0) { // Valid extent number
				// Valid extent number ==> valid extent
				uint32_t valid_extent_start = extent_block[cur_inode->extent_number[j] - 1].start;
				// Valid extent's start ==> valid resered data block
				struct a1fs_dentry *subdirectories = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * valid_extent_start);
				for (unsigned int k = 2; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) { // Iterate through 16 dentries
					if (subdirectories[k].ino < sb->inode_count && strcmp(subdirectories[k].name, components[i]) == 0) { // Component found
						block_number = valid_extent_start;
						offset = k;
						cur_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + subdirectories[k].ino * sizeof(struct a1fs_inode));
						j = 24;
						break;
					}
				}
			}
		}
	}

	int found = 0;
	uint32_t file_inode_number;
	
	/*check the file's inode*/
	for (int ii = 0; ii < 24; ii++) {
		if (cur_inode->extent_number[ii] > 0) {
			uint32_t valid_extent_start = extent_block[cur_inode->extent_number[ii] - 1].start;
			uint32_t valid_extent_count = extent_block[cur_inode->extent_number[ii] - 1].count;
			for (unsigned int jj = 0; jj < valid_extent_count; jj++) {
				struct a1fs_dentry *cur_dentry = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * (valid_extent_start + jj));
				for (unsigned int kk = 2; kk < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); kk++) {
					if (strcmp(cur_dentry[kk].name, components[number_of_components - 1]) == 0) { // component found
						file_inode_number = cur_dentry[kk].ino;
						offset = kk;
						block_number = valid_extent_start;
						found = 1;
						break;
						}
					}
				if (found) { // component found
					break;
				}
			}
		}
		if (found) { // coponent found
			break;
		}
	}
	
	/*Clear dentry in parent directory*/
	struct a1fs_dentry killer;
	killer.ino = sb->inode_count + 1; 
	killer.name[0] = '\0';
	memcpy(fs->image + A1FS_BLOCK_SIZE * block_number + offset * sizeof(a1fs_dentry), &killer, sizeof(a1fs_dentry));

	/*Clear bitmap*/
	unsigned char *inode_bitmap = fs->image + A1FS_BLOCK_SIZE;
	reset_bitmap(inode_bitmap, file_inode_number);
	unsigned char *block_bitmap = fs->image  + A1FS_BLOCK_SIZE * 2;
	struct a1fs_inode *file_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + sizeof(struct a1fs_inode) * file_inode_number);
	for (int index = 0; index < 24; index++) {
		if (file_inode->extent_number[index] > 0){//valid extent
			uint32_t extent_start = extent_block[file_inode->extent_number[index] - 1].start;
			uint32_t extent_count = extent_block[file_inode->extent_number[index] - 1].count;
			if (extent_start > 0){
				for (unsigned int count = 0; count < extent_count; count++) {
					reset_bitmap(block_bitmap, extent_start + count);
					memset(fs->image + (extent_start + count) * A1FS_BLOCK_SIZE, 0, A1FS_BLOCK_SIZE);
				}
			}
			struct a1fs_extent killer_extent;
			killer_extent.start = 0;
			killer_extent.count = 0;
			memcpy(fs->image + 3 * A1FS_BLOCK_SIZE + sizeof(struct a1fs_extent) * (file_inode->extent_number[index] - 1), &killer_extent, sizeof(struct a1fs_extent));
		}
		
	}

	/*Reset inode*/
	struct a1fs_inode killer_inode;
	killer_inode.links = 0; 
	killer_inode.size = 0;
	for (int kill = 0; kill < 24; kill++) {
		killer_inode.extent_number[kill] = 0;
	}
	memcpy(fs->image + A1FS_BLOCK_SIZE * 4 + file_inode_number * sizeof(struct a1fs_inode), &killer_inode, sizeof(struct a1fs_inode));
	return 0;
	
}

/**
 * Rename a file or directory.
 *
 * Implements the rename() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "from" exists.
 *   The parent directory of "to" exists and is a directory.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param from  original file path.
 * @param to    new file path.
 * @return      0 on success; -errno on error.
 */
static int a1fs_rename(const char *from, const char *to)
{
	fs_ctx *fs = get_fs();

	//Get a copy of path
	char from_cp[strlen(from) + 1];
	strncpy(from_cp, from, strlen(from));
	from_cp[strlen(from)] = '\0';
	char to_cp[strlen(to) + 1];
	strncpy(to_cp, to, strlen(to));
	to_cp[strlen(to)] = '\0';

	// Get the number of components
	int num_comp_from = count_components(from_cp);
	int num_comp_to = count_components(to_cp);

	// Get a list of components
	char *from_components[num_comp_from];
	char *to_components[num_comp_to];
	list_of_components(from_cp, from_components);
	list_of_components(to_cp, to_components);

	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	// Get the inode number for the last component in "from"
	// int from_ino = check_new(fs, num_comp_from, from_components);
	// Get the inode number for the last component in "to"
	int to_ino = check_new(fs, num_comp_to, to_components);
	// Get the inode for "from"
	// struct a1fs_inode *from_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + from_ino * sizeof(struct a1fs_inode));
	
	//Get "from"'s parent inode
	struct a1fs_inode *from_parent_inode;
	if (num_comp_from - 1 > 0) { // parent is not root
		int from_parent_inode_num = check_new(fs, num_comp_from - 1, from_components);
		from_parent_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + from_parent_inode_num * sizeof(struct a1fs_inode));
	} else if (num_comp_from - 1 == 0) { // parent is root
		from_parent_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4);
	}

	// Get "to"'s parent inode
	char to_parent_name[A1FS_NAME_MAX];
	struct a1fs_inode *to_parent_inode;
	if (num_comp_to - 1 > 0) { // parent is not root
		strcpy(to_parent_name, to_components[num_comp_to - 2]);
		to_parent_name[strlen(to_components[num_comp_to - 2])] = '\0';
		int to_parent_inode_num = check_new(fs, num_comp_to - 1, to_components);
		to_parent_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + to_parent_inode_num * sizeof(struct a1fs_inode));
	} else if (num_comp_to - 1 == 0) { // parent is root
		to_parent_name[0] = '/';
		to_parent_name[1] = '\0';
		to_parent_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4);
	}

	struct a1fs_dentry transfer_dentry;
	struct a1fs_dentry killer;
	// Delete the dentry in "from"'s parent inode
	for (int j = 0; j < 24; j++) { // Iterate through 24 extent numbers in the inode
		if (from_parent_inode->extent_number[j] > 0) { // Valid extent number
			// Valid extent number ==> valid extent
			uint32_t valid_extent_start = extent_block[from_parent_inode->extent_number[j] - 1].start;
			// Valid extent's start ==> valid resered data block
			
			struct a1fs_dentry *subdirectories = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * valid_extent_start);
			for (unsigned int k = 0; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) { // Iterate through 16 dentries
				if (subdirectories[k].ino < sb->inode_count && strcmp(subdirectories[k].name, from_components[num_comp_from - 1]) == 0) { // Component found
					/*copy the target's dentry*/
					transfer_dentry.ino = subdirectories[k].ino;
					transfer_dentry.name[0] = '\0';
					strcat(transfer_dentry.name, subdirectories[k].name);
					/*reset the dentry on the previous position*/
					killer.name[0] = '\0';
					killer.ino = sb->inode_count + 1;
					memcpy(fs->image + A1FS_BLOCK_SIZE * valid_extent_start + sizeof(struct a1fs_dentry) * k, &killer, sizeof(a1fs_dentry));
				}
			}	
		}
	}
	
	if (to_ino < (int)sb->inode_count && to_ino >= 0) { // If "to" exists
		struct a1fs_inode *to_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + to_ino * sizeof(struct a1fs_inode));
		if ((to_inode->mode & S_IFDIR) != S_IFDIR) { // If "to" is not directory
			return -ENOSPC;
		}
		// Find "to"'s dentry
		for (int j = 0; j < 24; j++) { // Iterate through 24 extent numbers in the inode
			if (to_inode->extent_number[j] > 0) { // Valid extent number
			// Valid extent number ==> valid extent
			uint32_t valid_extent_start = extent_block[to_inode->extent_number[j] - 1].start;
			// Valid extent's start ==> valid resered data block
			struct a1fs_dentry *subdirectories = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * valid_extent_start);
				for (unsigned int k = 0; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) { // Iterate through 16 dentries
					if (subdirectories[k].ino > sb->inode_count && strcmp(subdirectories[k].name, "") == 0) { // Component found
						// Found "to"'s parent dentry
						memcpy(fs->image + A1FS_BLOCK_SIZE * valid_extent_start + sizeof(struct a1fs_dentry) * k, &transfer_dentry, sizeof(struct a1fs_dentry));
						return 0;
					}
				}	
			}
		}
	} else if (to_ino < 0) { // If "to" does not exist
		// Find "to"'s parent dentry
		for (int j = 0; j < 24; j++) { // Iterate through 24 extent numbers in the inode
			if (to_parent_inode->extent_number[j] <= sb->reserved_extent_number && to_parent_inode->extent_number[j] > 0) { // Valid extent number
			// Valid extent number ==> valid extent
			uint32_t valid_extent_start = extent_block[to_parent_inode->extent_number[j] - 1].start;
			// Valid extent's start ==> valid resered data block
			struct a1fs_dentry *subdirectories = (struct a1fs_dentry *)(fs->image + A1FS_BLOCK_SIZE * valid_extent_start);
				for (unsigned int k = 0; k < A1FS_BLOCK_SIZE / sizeof(struct a1fs_dentry); k++) { // Iterate through 16 dentries
					if (subdirectories[k].ino > sb->inode_count && strcmp(subdirectories[k].name, "") == 0) { // Component found
						// Found "to"'s parent dentry
						a1fs_dentry new_transfer_dentry;
						new_transfer_dentry.ino = transfer_dentry.ino;
						new_transfer_dentry.name[0] = '\0';
						strcat(new_transfer_dentry.name, to_components[num_comp_to - 1]);
						memcpy(fs->image + A1FS_BLOCK_SIZE * valid_extent_start + sizeof(struct a1fs_dentry) * k, &new_transfer_dentry, sizeof(struct a1fs_dentry));
						return 0;
					}
				}	
			}
		}
	}
	return 0;
}


/**
 * Change the access and modification times of a file or directory.
 *
 * Implements the utimensat() system call. See "man 2 utimensat" for details.
 *
 * NOTE: You only have to implement the setting of modification time (mtime).
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists.
 *
 * @param path  path to the file or directory.
 * @param tv    timestamps array. See "man 2 utimensat" for details.
 * @return      0 on success; -errno on failure.
 */
static int a1fs_utimens(const char *path, const struct timespec tv[2])
{
	fs_ctx *fs = get_fs();
	
	/*Get a copy of path*/
	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);

	// Assign time
	int path_ino = check_new(fs, number_of_components, components);
	struct a1fs_inode *path_inode = (struct a1fs_inode *)(fs-> image + A1FS_BLOCK_SIZE * 4 + path_ino * sizeof(struct a1fs_inode));
	path_inode->mtime.tv_sec = tv[1].tv_sec;
	path_inode->mtime.tv_nsec = tv[1].tv_nsec;
	return 0;
}

/**
 * Change the size of a file.
 *
 * Implements the truncate() system call. Supports both extending and shrinking.
 * If the file is extended, future reads from the new uninitialized range must
 * return zero data.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to set the size.
 * @param size  new file size in bytes.
 * @return      0 on success; -errno on error.
 */
static int a1fs_truncate(const char *path, off_t size)
{
	fs_ctx *fs = get_fs();

	/*Get a copy of path*/
	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);
	
	// Find the inode of path file
	int path_ino = check_new(fs, number_of_components, components);
	struct a1fs_inode *path_inode = (struct a1fs_inode *)(fs-> image + A1FS_BLOCK_SIZE * 4 + path_ino * sizeof(struct a1fs_inode));
	
	struct a1fs_superblock *sb = (struct a1fs_superblock *)(fs->image);
	unsigned char *block_bitmap = fs->image  + A1FS_BLOCK_SIZE * 2;
	struct a1fs_extent *extent_block = (struct a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);
	// Get a list of meaningful extents in the inode
	struct a1fs_extent extents_map[24];
	for (int i = 0; i < 24; i++) {
		if (path_inode->extent_number[i] > 0) { 
			struct a1fs_extent valid_extent = extent_block[path_inode->extent_number[i] - 1];
			extents_map[i] = valid_extent;
		} else {
			struct a1fs_extent invalid_extent;
			invalid_extent.start = 0;
			invalid_extent.count = 0;
			extents_map[i] = invalid_extent;
		}
	}
	
	// Extending
	if ((unsigned long int)size > (unsigned long int)path_inode->size) {
		int requested_block = ceiling_block(size - path_inode->size);
		if ((uint64_t)requested_block > sb->free_data_block_count) { // blocks requested are too many
			return -ENOMEM;
		}
		// Need empty spots for new extents
		int num_empty_spot = 0;
		bool found_empty_spot = false;
		for (int i = 0; i < 24; i++) {
			if (extents_map[i].start == 0 && extents_map[i].count == 0) { // Found an empty spot
				found_empty_spot = true;
				num_empty_spot++;
			}
 		}
		// None of the spot is empty
		if (!found_empty_spot) { 
			return -ENOSPC;
		}
		int zero_pos[A1FS_BLOCK_SIZE * 8];
		find_zero_postions(fs, zero_pos);
		// Create extents
		for (int j = 0; j < 24; j++) { 
			if (extents_map[j].start == 0 && extents_map[j].count == 0) { // found empty extent
				int k = 0;
				while (k < requested_block) { // Still requesting
					// Create a extent
					struct a1fs_extent new_extent;
					new_extent.start = zero_pos[k] + 4 + sb->inode_blocks;
					new_extent.count = 1;
					//Change superblock
					sb->reserved_extent_number++;
					sb->free_data_block_count--;
					// Fill data block with zero data
					memset(fs->image + new_extent.start * A1FS_BLOCK_SIZE, 0, A1FS_BLOCK_SIZE);
					// Flip bitmap
					assign_bitmap(block_bitmap, zero_pos[k]);
					// Reserve a slot in inode
					extents_map[j] = new_extent;
					int previous = zero_pos[k];
					k++;
					while (k < requested_block) { // Still requesting
						if (zero_pos[k] == previous + 1) { // consecutive 0s should be in one extent
							new_extent.count++;
							//Change superblock
							sb->free_data_block_count--;
							// Fill data block with zero data
							memset(fs->image + (zero_pos[k] + 4 + sb->inode_blocks) * A1FS_BLOCK_SIZE, 0, A1FS_BLOCK_SIZE);
							// Flip bitmap
							assign_bitmap(block_bitmap, zero_pos[k]);
							previous = zero_pos[k];
							k++;
						} else { // non-consecutive 0s should not be in one extent
							break;
						}
					}
					memcpy(fs->image + A1FS_BLOCK_SIZE * 3 + (sb->reserved_extent_number - 1) * sizeof(struct a1fs_extent), &new_extent, sizeof(struct a1fs_extent));
					// Add extent number to the inode
					for (int index = 0; index < 24; index++) {
						if (path_inode->extent_number[index] == 0){
							path_inode->extent_number[index] = sb->reserved_extent_number;
							break;
						}
					}
				}
				// Extend finished when every requested block is assigned to a extent
				if (k == requested_block) {
					path_inode->size = size;
					return 0;
				}
			}
		}
	}
	// Shrinking
	if ((unsigned long int)size < (unsigned long int)path_inode->size) {
		int expected_num_blocks = ceiling_block(size);
		int cur_num_blocks = ceiling_block(path_inode->size);
		int num_blocks_to_shrink = cur_num_blocks - expected_num_blocks;
		unsigned int ending = path_inode->size % A1FS_BLOCK_SIZE;
		if (num_blocks_to_shrink == 0 && (ending > (path_inode->size - size))) { // Ex: 8192 => 5000 is 2 - 2
			// Fill positions with zeros
			for (int x = 23; x >= 0; x--) {
				if (extents_map[x].start != 0 && extents_map[x].count != 0) {
					//clear file
					memset(fs->image + A1FS_BLOCK_SIZE * (extents_map[x].start + extents_map[x].count - 1) + path_inode->size - size, 0, A1FS_BLOCK_SIZE - path_inode->size + size);
					break;
				}
			}
			path_inode->size = size;
			return 0;
		} else { // Ex: 8192 => 3000 is 2 - 1 or 8193 => 7000 is 3 - 2
			if (!num_blocks_to_shrink) {
				num_blocks_to_shrink ++;
			}
			for (int i = 23; i >= 0; i--) {
				if (extents_map[i].start != 0 && extents_map[i].count != 0) { // The last meaningful extent
					while (extents_map[i].count > 0) {
						// Set this data block to zero
						memset(fs->image + A1FS_BLOCK_SIZE * (extents_map[i].start + extents_map[i].count - 1), 0, A1FS_BLOCK_SIZE);
						// Change superblock
						sb->free_data_block_count++;
						// Reset bitmap
						reset_bitmap(block_bitmap, extents_map[i].start + extents_map[i].count - 1);
						// Decrement the count of this extent.count
						extents_map[i].count--;
						num_blocks_to_shrink--;
						if (extents_map[i].count == 0) { // This extent is already empty
							// Change superblock
							sb->reserved_extent_number--;
							// Make this extent empty
							extents_map[i].start = 0;
							path_inode->extent_number[i] = 0;
						}
						// Need to check is shrink finished after removing a block that should be removed
						if (num_blocks_to_shrink == 0) {
							path_inode->size = size;
							return 0;
						}
					}
				}
			}
		}
	}
	return 0;
}


/**
 * Read data from a file.
 *
 * Implements the pread() system call. Should return exactly the number of bytes
 * requested except on EOF (end of file) or error, otherwise the rest of the
 * data will be substituted with zeros. Reads from file ranges that have not
 * been written to must return zero data.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * @param path    path to the file to read from.
 * @param buf     pointer to the buffer that receives the data.
 * @param size    buffer size - number of bytes requested.
 * @param offset  offset from the beginning of the file to read from.
 * @param fi      unused.
 * @return        number of bytes read on success; 0 if offset is beyond EOF;
 *                -errno on error.
 */
static int a1fs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);

	int inode_number = check_new(fs, number_of_components, components);

	/* Get the inode & extent block */
	a1fs_inode *dest_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + inode_number * sizeof(struct a1fs_inode));
	a1fs_extent *extent_block = (a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);

	// calculate the offset
	int offset_block = offset / A1FS_BLOCK_SIZE;
	int remaining = offset % A1FS_BLOCK_SIZE;

	// get the starting point of read
	int block_number[dest_inode->size / A1FS_BLOCK_SIZE + 1];
	block_number[dest_inode->size / A1FS_BLOCK_SIZE] = 0;
	int pos = 0;
	for (int i = 0; i < 24; i++) {
		if (dest_inode->extent_number[i] > 0) {
			a1fs_extent extent = extent_block[dest_inode->extent_number[i] - 1];
			if (extent.start != 0) {
				for (unsigned int c = 0; c < extent.count; c++) {
					block_number[pos] = extent.start + extent.count - 1;
					pos++;
				}
			}	
		}
	}

	// copy the data to the buf
	unsigned char *destination = (unsigned char *) fs->image + A1FS_BLOCK_SIZE * block_number[offset_block];
	for (unsigned int j = 0; j < size; j++) {
		buf[j] = destination[remaining];
		remaining ++;
		if (remaining == A1FS_BLOCK_SIZE) {
			remaining = 0;
			offset_block++;
			destination = (unsigned char *) fs->image + A1FS_BLOCK_SIZE * block_number[offset_block];
		}
	}

	return size;
}

/**
 * Write data to a file.
 *
 * Implements the pwrite() system call. Should return exactly the number of
 * bytes requested except on error. If the offset is beyond EOF (end of file),
 * the file must be extended. If the write creates a "hole" of uninitialized
 * data, future reads from the "hole" must return zero data.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * @param path    path to the file to write to.
 * @param buf     pointer to the buffer containing the data.
 * @param size    buffer size - number of bytes requested.
 * @param offset  offset from the beginning of the file to write to.
 * @param fi      unused.
 * @return        number of bytes written on success; -errno on error.
 */
static int a1fs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	char path_cp[strlen(path) + 1];
	strncpy(path_cp, path, strlen(path));
	path_cp[strlen(path)] = '\0';

	// Get the number of components
	int number_of_components = count_components(path_cp);

	// Get a list of components
	char *components[number_of_components];
	list_of_components(path_cp, components);

	int inode_number = check_new(fs, number_of_components, components);

	/*Get the inode & extent block*/
	a1fs_inode *dest_inode = (struct a1fs_inode *)(fs->image + A1FS_BLOCK_SIZE * 4 + inode_number * sizeof(struct a1fs_inode));
	a1fs_extent *extent_block = (a1fs_extent *)(fs->image + A1FS_BLOCK_SIZE * 3);

	//check if it is necessary to extend
	if (offset + size > dest_inode->size){
		if (a1fs_truncate(path, offset + size) < 0){
			return -ENOSPC;
		}
	}

	// calculate the offset
	int offset_block = offset / A1FS_BLOCK_SIZE;
	int remaining = offset % A1FS_BLOCK_SIZE;

	// get the starting point of read
	int block_number[dest_inode->size / A1FS_BLOCK_SIZE + 1];
	block_number[dest_inode->size / A1FS_BLOCK_SIZE] = 0;
	int pos = 0;
	for (int i = 0; i < 24; i++){
		if (dest_inode->extent_number[i] > 0){
			a1fs_extent extent = extent_block[dest_inode->extent_number[i] - 1];
			if (extent.start != 0){
				for (unsigned int c = 0; c < extent.count; c++){
					block_number[pos] = extent.start + extent.count - 1;
					pos++;
				}
			}	
		}	
	}

	// copy the data to the data block
	unsigned char *destination = (unsigned char *) fs->image + A1FS_BLOCK_SIZE * block_number[offset_block];
	for (unsigned int j = 0; j < size; j++){
		destination[remaining] = buf[j];
		remaining ++;
		if (remaining == A1FS_BLOCK_SIZE){
			remaining = 0;
			offset_block++;
			destination = (unsigned char *) fs->image + A1FS_BLOCK_SIZE * block_number[offset_block];
		}
	}

	return size;
}


static struct fuse_operations a1fs_ops = {
	.destroy  = a1fs_destroy,
	.statfs   = a1fs_statfs,
	.getattr  = a1fs_getattr,
	.readdir  = a1fs_readdir,
	.mkdir    = a1fs_mkdir,
	.rmdir    = a1fs_rmdir,
	.create   = a1fs_create,
	.unlink   = a1fs_unlink,
	.rename   = a1fs_rename,
	.utimens  = a1fs_utimens,
	.truncate = a1fs_truncate,
	.read     = a1fs_read,
	.write    = a1fs_write,
};

/*Search the empty blocks*/


int main(int argc, char *argv[])
{
	a1fs_opts opts = {0};// defaults are all 0
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	if (!a1fs_opt_parse(&args, &opts)) return 1;

	fs_ctx fs = {0};
	if (!a1fs_init(&fs, &opts)) {
		fprintf(stderr, "Failed to mount the file system\n");
		return 1;
	}

	return fuse_main(args.argc, args.argv, &a1fs_ops, &fs);
}
