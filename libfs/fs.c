#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "disk.h"
#include "fs.h"

#define SUPERBLOCK_SIG_LEN 8
#define SUPERBLOCK_PREPAD_LEN SUPERBLOCK_SIG_LEN+2+2+2+2+1
#define SUPERBLOCK_PAD_LEN BLOCK_SIZE-SUPERBLOCK_PREPAD_LEN

#define ROOT_PAD_LEN 10
#define ROOT_ENTRY_LEN 32

#define FAT_EOC 0xFFFF

#define DEBUG_MODE 1

struct SuperBlock{
	uint8_t signature[8];
	uint16_t num_blocks;
	uint16_t root_index;
	uint16_t data_start_index;
	uint16_t num_data_blocks;
	uint8_t num_fat_blocks;
	uint8_t padding[SUPERBLOCK_PAD_LEN];
};

struct File{
	uint8_t filename[FS_FILENAME_LEN];
	uint32_t file_size;
	uint16_t first_index;
	uint8_t padding[ROOT_PAD_LEN];
};

struct FileDescriptor{
	struct File* file;
	uint16_t offset;
};

/* TODO: Phase 1 */

struct SuperBlock* sb;
uint16_t* fat;
struct File* root[FS_FILE_MAX_COUNT];
struct FileDescriptor* open_files[FS_OPEN_MAX_COUNT];
uint8_t num_open_files = 0;
bool is_disk_mounted = false;

// debug tool
// prints every byte in a block, defined as BLOCK_SIZE lengthed byte array
void printblock(uint8_t* block){
	for (unsigned i = 0; i < BLOCK_SIZE; i++){
		printf("%d ", *(block+i));
	}

	printf("\n\n");
}

// return 0 if the strings are equal
// return -1 if the strings are not equal
int streq(const uint8_t* str1, const uint8_t* str2){
	for (unsigned i = 0; i < FS_FILENAME_LEN; i++){
		if (str1[i] == '\0' && str2[i] == '\0'){
			return 0;
		} else if (str1[i] != str2[i]){
			return -1;
		}
	}

	return -1;
}

// used to convert two one-byte nums into one two-byte num
// uses little endian
uint16_t concatenate_two_bytes(uint8_t byte1, uint8_t byte2){
	uint16_t result = 0;
	result += 256*byte2;
	result += byte1;
	return result;
}

// same as above, but for a four-byte num
uint32_t concatenate_four_bytes(uint8_t byte1, uint8_t byte2, uint8_t byte3, uint8_t byte4){
	uint32_t result = 0;
	result += pow(2, 24)*byte4;
	result += pow(2, 16)*byte3;
	result += pow(2, 8)*byte2;
	result += byte1;
	return result;
}

// evaluates if a filename is invalid, either because it is null, the first char is null, or it is too long
bool is_filename_invalid(const char* filename){
	return filename == NULL || *filename == '\0' || strlen(filename)+1 > FS_FILENAME_LEN;
}

// helper function for fs_mount()
// attempt to open the disk
// returns 0 on success and -1 on failure
// flags is_disk_mounted, which is used in many functions as an error check
int load_disk(const char* diskname){
	if (is_disk_mounted){
		// fprintf(stderr, "Error in fs_mount(): disk already mounted\n");
		return -1;
	}
	int open_disk_success = block_disk_open(diskname);
	// printf("disk success = %d\n", open_disk_success);
	if (open_disk_success != 0){
		// fprintf(stderr, "Error in fs_mount(): could not read superblock\n");
		return -1;
	}

	is_disk_mounted = true;
	return 0;
}

// helper function for fs_mount()
// loads in the superblock info
int load_superblock(uint8_t* superblock_ptr){
	sb = malloc(sizeof(struct SuperBlock));
	if (superblock_ptr == NULL){
		// fprintf(stderr, "Error in fs_mount(): pointer to superblock is null\n");
		return -1;
	}

	// load each byte of the signature in one at a time
	for (unsigned i = 0; i < SUPERBLOCK_SIG_LEN; i++){
		sb->signature[i] = *(superblock_ptr++);
	}

	// this should concatenate the next two bytes of the buffer
	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb->num_blocks = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb->num_blocks != block_disk_count()){
		// fprintf(stderr, "Error in fs_mount(): num_blocks read from superblock does not match number on disk\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb->root_index = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb->root_index <= 0){
		// fprintf(stderr, "Error in fs_mount(): root index is where superblock should be\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb->data_start_index = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb->data_start_index == 0){
		// fprintf(stderr, "Error in fs_mount(): data start index is where superblock should be\n");
		return -1;
	}

	if (sb->data_start_index == sb->root_index){
		// fprintf(stderr, "Error in fs_mount(): root and data start have the same index\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb->num_data_blocks = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb->num_data_blocks >= sb->num_blocks){
		// fprintf(stderr, "Error in fs_mount(): more data blocks than total blocks\n");
		return -1;
	}

	// printf("%d\n", *superblock_ptr);
	sb->num_fat_blocks = *(superblock_ptr);
	superblock_ptr++;

	if (sb->num_fat_blocks >= sb->num_blocks){
		// fprintf(stderr, "Error in fs_mount(): more FAT blocks than total blocks\n");
		return -1;
	}

	for (unsigned i = SUPERBLOCK_PREPAD_LEN; i < BLOCK_SIZE; i++){
		sb->padding[i] = *(superblock_ptr++);

		if (sb->padding[i] != 0){
			// fprintf(stderr, "Error in fs_mount(): incorrect padding formatting\n");
			return -1;
		}
	}

	return 0;
}

// helper function for fs_mount()
// loads the FAT data
int load_fat(){
	fat = malloc(sb->num_data_blocks * sizeof(uint16_t));
	if (fat == NULL){
		// fprintf(stderr, "Error in load_fat(): could not allocate fat block\n");
		return -1;
	}

	// buf_size is the length of the buffer used to load in the FAT data
	// since the length of the FAT is equal to the number of data blocks times the size of 2-bytes
	// since each entry in the FAT is 2 bytes
	// buf_size should normally be that long
	// but if there are not many entries for the FAT, the buffer should at least be one block long
	// so block_read will work
	uint16_t buf_size;
	if (sb->num_data_blocks*sizeof(uint16_t) > BLOCK_SIZE){
		buf_size = sb->num_data_blocks * sizeof(uint16_t);
	} else {
		buf_size = BLOCK_SIZE;
	}

	// printf("buf size = %d\n", buf_size);
	// printf("num fat blocks = %d\n", sb->num_fat_blocks);

	// read in the FAT blocks into the buffer
	uint16_t buffer[buf_size];
	int read_success;
	for (unsigned i = 0; i < sb->num_fat_blocks; i++){
		// printf("i+1=%d\n", i+1);
		// printf("BLOCK_SIZE * %d = %d\n", i, BLOCK_SIZE*i);
		read_success = block_read(i+1, &buffer[BLOCK_SIZE*i]);
		if (read_success == -1){
			// fprintf(stderr, "Error in load_fat: failed to read block at index %d\n", i+1);
			return -1;
		}
	}

	// load each entry from the FAT blocks into the FAT
	for (uint16_t i = 0; i < sb->num_data_blocks; i++){
		*(fat+i) = buffer[i];
		// printf("%d\n", *(fat+i));
	}

	return 0;
}

// helper function for fs_mount()
// loads in the root info
int load_root_directory(uint8_t* root_ptr){
	// printf("root dir\n");

	// root is an array of file pointers
	for (unsigned f = 0; f < FS_FILE_MAX_COUNT; f++){
		struct File* file = malloc(sizeof(struct File));

		// load in each byte of the name individually
		// each char is one byte
		for (unsigned i = 0; i < FS_FILENAME_LEN; i++){
			file->filename[i] = *(root_ptr++);
		}

		// load in the file size (4 bytes)
		file->file_size = concatenate_four_bytes(*root_ptr, *(root_ptr+1), *(root_ptr+2), *(root_ptr+3));
		root_ptr += 4;

		// load in the first index (2 bytes)
		file->first_index = concatenate_two_bytes(*root_ptr, *(root_ptr+1));
		root_ptr += 2;

		// load in the padding (10 bytes)
		for (unsigned i = 0; i < ROOT_PAD_LEN; i++){
			file->padding[i] = (*root_ptr++);
		}

		root[f] = file;
		// printf("in load_root_directory: %d\n", root->files[f]->first_index);
	}

	return 0;
}

// helper function for fs_create()
// finds the first empty entry in the root directory and returns its index
// returns -1 if unable to find empty entry
int find_empty_root_entry(){
	for (unsigned i = 0; i < FS_FILENAME_LEN; i++){
		if (root[i]->filename[0] == 0){
			return i;
		}
	}

	return -1;
}


// helper function for various functions
// returns -1 if there is no matching filename in the root directory
// returns the file index if there is a matching filename
// no error checking here because that is done before it is called
int find_matching_filename(const char* filename){
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		if (strcmp(filename, (char*)(root[i]->filename)) == 0){
			return i;
		}
	}

	return -1;
}

// helper function for fs_open()
// allocates memory for a FileDescriptor and adds it to the FD array
// the index of the file in the array is its FD num, the one used in many func params
int add_file_to_fd_array(struct File* file){
	if (num_open_files > FS_OPEN_MAX_COUNT){
		return -1;
	}
	
	// initialize FD
	struct FileDescriptor* fd = malloc(sizeof(struct FileDescriptor));
	fd->file = file;
	fd->offset = 0;
	
	// find empty entry in the FD array and add the file there
	int fd_index = -1;
	for (unsigned i = 0; i < FS_OPEN_MAX_COUNT; i++){
		if (open_files[i] == NULL){
			open_files[i] = fd;
			fd_index = i;
			num_open_files++;
			break;
		}
	}

	return fd_index;
}

// helper function for fs_write() and fs_read()
// finds the number of blocks required to access for the write/read
int find_num_target_blocks(uint16_t offset, size_t count){
	if (offset + count <= BLOCK_SIZE){
		return 1;
	}

	/*
	scenario - offset is 3 blocks + 1 partial block. lets say 4096*3 + 1000
	count is 4 blocks + 1 partial block. lets say 4096*4 + 3000
	the return of this func should include the 3 blocks we dont write to
	starting indexing at block 0,
	we start at byte 1000 of block 3
	we then write from byte 1000 of block 3 to byte 1000 of block 7
	then we write to byte 4000 of block 7
	it should return 8
	however, if offset % BLOCK_SIZE + count % BLOCK_SIZE > BLOCK_SIZE, it should return 1 more (9)
	
	let offset = m*B + o
	let count = n*B + c
	num full blocks = m+n
	*/
	int num_target_blocks = 0;
	if (offset != 0){
		// in our scenario, 0+3 = 3
		num_target_blocks += offset / BLOCK_SIZE;
		if (offset % BLOCK_SIZE != 0){
			// 3+1 = 4
			num_target_blocks++;
		}
	}

	// 4 + 4 = 8
	num_target_blocks += count / BLOCK_SIZE;

	if (offset % BLOCK_SIZE + count % BLOCK_SIZE != 0){
		num_target_blocks++;
	}

	return num_target_blocks;
}

// helper function for fs_write()
int allocate_blocks_in_fat(const int fd, const int num_target_blocks){
	// printf("running allocate_blocks_in_fat\n");

	if (num_target_blocks <= 0){
		return 0;
	}

	uint16_t block_index = open_files[fd]->file->first_index;

	// if our first index is FAT_EOC, then the file has no data blocks allocated
	// so we iterate through the FAT until we find an empty entry, then make that the file's first block
	if (block_index == FAT_EOC){
		for (int i = 1; i < sb->num_data_blocks; i++){
			if (*(fat+i) == 0){
				*(fat+i) = FAT_EOC;
				block_index = i;
				open_files[fd]->file->first_index = block_index;
				break;
			}
		}

		if (block_index == FAT_EOC){
			fprintf(stderr, "Error in allocate_blocks_in_fat(): unable to find empty FAT entry for block 0\n");
			return -1;
		}
	}
	int num_data_blocks = 1;

	// we then follow the chain of data blocks for the file until we reach the end
	// the loop exits when fat[block_index] == FAT_EOC. this will leave us with the last valid index
	for (block_index = open_files[fd]->file->first_index; *(fat+block_index) != FAT_EOC; num_data_blocks++){
		block_index = *(fat+block_index);
	}

	// printf("num data blocks at start is %d\n", num_data_blocks);

	// now we extend the chain out to our desired length
	while (num_data_blocks < num_target_blocks){
		int found_new_index = 0;
		// we start at 1 because fat[0] is always invalid
		for (int i = 1; i < sb->num_data_blocks; i++){
			// 0 signifies an empty entry available to incorporate into our file's linked list
			if (*(fat+i) == 0){
				// printf("block index = %d\n", block_index);
				*(fat+block_index) = i;
				*(fat+i) = FAT_EOC;
				num_data_blocks++;
				block_index = i;
				found_new_index = 1;
				break;
			}
		}

		if (!found_new_index){
			fprintf(stderr, "Error in allocate_blocks_in_fat(): unable to find empty FAT entry for block %d\n", num_data_blocks);
			return -1;
		}
	}

	// printf("end of allocate_blocks_in_fat\nResults:\nblock index = %d\nnum data blocks = %d\nnum target blocks = %d\n", block_index, num_data_blocks, num_target_blocks);
	return 0;
}

// find the index in the FAT of the nth block in the file
// returns FAT_EOC if the request was out of bounds
uint16_t find_data_block(const int fd, const int block_num){
	uint16_t first_index = open_files[fd]->file->first_index;
	// printf("first index = %d\n", first_index);
	if (block_num == 0){
		return first_index;
	}

	uint16_t block_index = first_index;
	for (int i = 0; i < block_num; i++){
		if (block_index == FAT_EOC){
			fprintf(stderr, "Error in find_data_block(): request is out of bounds for the file\n");
			return FAT_EOC;
		}
		block_index = *(fat+block_index);
		// printf("find data block: block#%d in the file is at index %d\n", i, block_index);
	}

	return block_index;
}

int fs_mount(const char *diskname)
{
	/* TODO: Phase 1 */
	// printf("starting fs_mount\n");

	// open the disk
	if (load_disk(diskname) != 0){
		// fprintf(stderr, "Error in fs_mount(): could not open disk\n");
		return -1;
	}

	// load the buffer containing the superblock data
	uint8_t buffer[BLOCK_SIZE];
	void* buf_ptr = &buffer;

	int read_success = block_read(0, buf_ptr);
	// printf("read superblock %d\n", read_success);
	if (read_success != 0){
		return -1;
	}


	uint8_t* superblock_ptr = (uint8_t*)buf_ptr;

	if (load_superblock(superblock_ptr) != 0){
		return -1;
	}
	
	if (load_fat() != 0){
		return -1;
	}

	read_success = block_read(sb->num_fat_blocks+1, buf_ptr);
	uint8_t* root_ptr = (uint8_t*)buf_ptr;
	if (load_root_directory(root_ptr) != 0){
		return -1;
	}

	for (unsigned i = 0; i < FS_OPEN_MAX_COUNT; i++){
		open_files[i] = NULL;
	}

	return 0;
}

int fs_umount(void)
{
	/* TODO: Phase 1 */
	// printf("running unmount\n");
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_unmount(): no disk is mounted\n");
		return -1;
	}

	if (num_open_files > 0){
		fprintf(stderr, "Error in fs_unmount(): cannot unmount until all files are closed\n");
	}

	// printf("first index as read by unmount: %d\n", root[0]->first_index);
	// uint8_t buffer[BLOCK_SIZE];
	// block_read(root[0]->first_index, buffer);
	// printblock(buffer);

	// write to the FAT blocks and root block to save changes
	for (uint16_t i = 0; i < sb->num_fat_blocks; i++){
		block_write(i+1, fat+BLOCK_SIZE*i);
	}

	// to write to the root block, we need to convert the root into a flat byte array
	uint8_t root_array[BLOCK_SIZE];
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){

		// this offset lets us work with the current root entry
		int offset = ROOT_ENTRY_LEN*i;
		struct File* file = root[i];

		// load each byte of the file's name in
		for (unsigned j = 0; j < FS_FILENAME_LEN; j++){
			root_array[offset+j] = file->filename[j];
		}

		offset += FS_FILENAME_LEN;

		// file size is 4 bytes
		// this operation lets us isolate each byte of the num
		// we store them in reverse order because of little endianness
		root_array[offset+3] = (file->file_size >> 24) & 0xFF;
		root_array[offset+2] = (file->file_size >> 16) & 0xFF;
		root_array[offset+1] = (file->file_size >> 8) & 0xFF;
		root_array[offset+0] = (file->file_size >> 0) & 0xFF;

		offset += 4;

		// we do the same for the first index, but only for 2 bytes
		root_array[offset+1] = (file->first_index >> 8) & 0xFF;
		root_array[offset+0] = (file->first_index >> 0) & 0xFF;

		offset += 2;

		// store each byte of the padding
		for (unsigned j = 0; j < ROOT_PAD_LEN; j++){
			root_array[offset+j] = file->padding[j];
		}
	}

	// now that we have a flat array, we can easily write it to the root block
	block_write(sb->root_index, root_array);

	// free the memory we allocated in fs_mount()
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		free(root[i]);
	}

	free(fat);
	free(sb);

	// close the disk
	block_disk_close();

	is_disk_mounted = false;
	return 0;
}

int fs_info(void)
{
	/* TODO: Phase 1 */
	// printf("running info\n");
	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_info(): no disk is mounted\n");
		return -1;
	}

	printf("FS Info:\n");
	printf("total_blk_count=%d\n", block_disk_count());
	printf("fat_blk_count=%d\n", sb->num_fat_blocks);
	printf("rdir_blk=%d\n", sb->root_index);
	printf("data_blk=%d\n", sb->data_start_index);
	printf("data_blk_count=%d\n", sb->num_data_blocks);

	// free fat entries means the number of entries in the FAT that equal 0
	// in other words, how many data blocks do not belong to a file
	int free_fat_entries = 0;
	for (uint16_t i = 0; i < sb->num_data_blocks; i++){
		if (*(fat+i) == 0){
			free_fat_entries++;
		}
	}

	printf("fat_free_ratio=%d/%d\n", free_fat_entries, sb->num_data_blocks);

	// free root entries means the number of possible files that have not been created
	int free_root_entries = 0;
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		if (root[i]->filename[0] == '\0'){
			free_root_entries++;
		} else {

			// uint8_t buffer[BLOCK_SIZE];
			// block_read(root[i]->first_index, buffer);
			// for (int i = 0; i < BLOCK_SIZE; i++){
			// 	printf("%c", buffer[i]);
			// }
		}
	}

	printf("rdir_free_ratio=%d/%d\n", free_root_entries, FS_FILE_MAX_COUNT);
	
	return 0;
}

int fs_create(const char *filename)
{
	/* TODO: Phase 2 */
	// printf("running create\n");
	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_create(): no disk is mounted\n");
		return -1;
	}
	if (is_filename_invalid(filename)){
		// fprintf(stderr, "Error in fs_create(): invalid file name\n");
		return -1;
	}

	if (find_matching_filename(filename) != -1){
		// fprintf(stderr, "Error in fs_create(): file named %s already exists\n", filename);
		return -1;
	}

	// find new entry
	int empty_index = find_empty_root_entry();
	if (empty_index == -1){
		// fprintf(stderr, "Error in fs_create(): unable to find empty index\n");
		return -1;
	}
	struct File* new_file = root[empty_index];

	// since new_file->filename is an array of uint8_t,
	// and filename is a char*,
	// we must fill up the array byte-by-byte instead of all at once
	for (unsigned i = 0; i < strlen(filename); i++){
		new_file->filename[i] = filename[i];
	}

	new_file->filename[strlen(filename)] = '\0';

	// reset the other members of the struct
	new_file->file_size = 0;
	new_file->first_index = FAT_EOC;
	// printf("in fs_create: %d\n", root[empty_index]->first_index);

	// printf("%s\n", filename);
	return 0;
}

int fs_delete(const char *filename)
{
	/* TODO: Phase 2 */
	// printf("runnin delete\n");
	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_delete(): no disk is mounted\n");
		return -1;
	}

	if (is_filename_invalid(filename)){
		// fprintf(stderr, "Error in fs_delete(): invalid file name\n");
		return -1;
	}

	int matching_file_index = find_matching_filename(filename);
	if (matching_file_index == -1){
		// fprintf(stderr, "Error in fs_create(): file named %s does not exist\n", filename);
		return -1;
	}

	// check if the requested file is opened, and reject it if it is
	for (unsigned i = 0; i < FS_OPEN_MAX_COUNT; i++){
		if (open_files[i] != NULL && streq(open_files[i]->file->filename, (uint8_t*)filename) == 0){
			// fprintf(stderr, "Error in fs_delete(): open files cannot be deleted\n");
			return -1;
		}
	}

	struct File* old_file = root[matching_file_index];

	// free all of the data blocks in the FAT the file was using
	uint16_t block_index = old_file->first_index;
	if (block_index != FAT_EOC){
		while (*(fat+block_index) != FAT_EOC){
			uint16_t old_block_index = block_index;
			*(fat+old_block_index) = 0;
			block_index = *(fat+block_index);
		}

		*(fat+block_index) = 0;
	}

	// initialize the members of the file to be an empty entry
	old_file->filename[0] = '\0';
	old_file->file_size = 0;
	old_file->first_index = FAT_EOC;

	return 0;
}

int fs_ls(void)
{
	/* TODO: Phase 2 */
	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_ls(): no disk is mounted\n");
	}

	printf("FS Ls:\n");
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		struct File* file = root[i];
		if (file->filename[0] != 0){
			// printf("File #%d\n", i);
			// printf("Name: %s\n", root[i]->filename);
			// printf("Size: %d\n", root[i]->file_size);
			// printf("First index: %d\n", root[i]->first_index);
			printf("file: %s, size: %d, data_blk: %d\n", file->filename, file->file_size, file->first_index);
		}
	}
	return 0;
}

// this function is very simple, since most of the heavy lifting is done in helper functions
int fs_open(const char *filename)
{
	/* TODO: Phase 3 */
	// printf("running open\n");
	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_open(): no disk is mounted\n");
		return -1;
	}

	if (is_filename_invalid(filename)){
		// fprintf(stderr, "Error in fs_create(): invalid file name\n");
		return -1;
	}

	int file_index = find_matching_filename(filename);
	if (file_index == -1){
		// fprintf(stderr, "Error in fs_open(): file %s not found\n", filename);
		return -1;
	}

	int fd = add_file_to_fd_array(root[file_index]);
	if (fd == -1){
		// fprintf(stderr, "Error in fs_open(): max number of files already open\n");
		return -1;
	}

	return fd;
}

int fs_close(int fd)
{
	/* TODO: Phase 3 */
	// printf("running close\n");
	// printf("first index as read by fs close: %d\n", open_files[fd]->file->first_index);
	// uint8_t buffer[BLOCK_SIZE];
	// block_read(open_files[fd]->file->first_index, buffer);
	// printblock(buffer);

	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_close(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		// fprintf(stderr, "Error in fs_close(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		// fprintf(stderr, "Error in fs_close(): file with descriptor %d not open\n", fd);
		return -1;
	}

	// free the memory we allocated in fs_create()
	free(open_files[fd]);
	open_files[fd] = NULL;
	num_open_files--;
	return 0;
}

int fs_stat(int fd)
{
	/* TODO: Phase 3 */
	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_stat(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		// fprintf(stderr, "Error in fs_stat(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		// fprintf(stderr, "Error in fs_stat(): file with descriptor %d not open\n", fd);
		return -1;
	}

	return open_files[fd]->file->file_size;
}

int fs_lseek(int fd, size_t offset)
{
	/* TODO: Phase 3 */
	// printf("running lseek\n");
	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_lseek(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		// fprintf(stderr, "Error in fs_lseek(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		// fprintf(stderr, "Error in fs_lseek(): file with descriptor %d not open\n", fd);
		return -1;
	}

	if (offset > open_files[fd]->file->file_size){
		// fprintf(stderr, "Error in fs_lseek(): specified offset too large (%ld vs %d)\n", offset, open_files[fd]->file->file_size);
		return -1;
	}

	open_files[fd]->offset = offset;
	return 0;
}

int fs_write(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
	// printf("running fs_write\n");

	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_write(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		// fprintf(stderr, "Error in fs_write(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		// fprintf(stderr, "Error in fs_write(): file with descriptor %d not open\n", fd);
		return -1;
	}

	if (buf == NULL){
		// fprintf(stderr, "Error in fs_write(): buf is null\n");
		return -1;
	}

	// if there is nothing to write, dont bother with anything
	if (count == 0){
		return 0;
	}

	uint16_t offset = open_files[fd]->offset;

	// used for more readable error checking
	int op_success = 0;

	int bytes_written = 0;

	// the index of the first block we will write fully to
	uint16_t full_block_start_index = 0;

	// the number of blocks we will be writing to
	// this includes blocks skipped by the offset
	int num_target_blocks = find_num_target_blocks(offset, count);
	// printf("write\noffset = %d\ncount = %ld\nnum target blocks = %d\n", offset, count, num_target_blocks);
	
	// if the file does not have enough blocks registered, allocate more of them
	op_success = allocate_blocks_in_fat(fd, num_target_blocks);
	if (op_success == -1){
		// if this fails, then there are not enough blocks to complete the write
		// however, the function will continue and write as much as it can
		fprintf(stderr, "Error in fs_write(): block allocation failed\n");
	}

	// partial first block (if applicable)
	if (offset != 0){
		// uint16_t first_index = open_files[fd]->file->first_index;

		/*
		scenarios:
			if offset < BLOCK_SIZE, then offset / BLOCK_SIZE = 0
			find data block will return the index of the first block of the file (block 0)
			this is what we want

			if offset >= BLOCK_SIZE, then offset / BLOCK_SIZE > 0 = n
			find data block will return the index of the nth block of the file
			eg if offset / BLOCK SIZE == 1, FDB will return the index of block 1
			this is what we want
			
			offset % BLOCK SIZE does not matter for this part
		*/

		uint16_t first_index = find_data_block(fd, offset / BLOCK_SIZE);

		// first_index will be FAT_EOC if offset / BLOCK SIZE is out of bounds
		// since we do not have room to write anything, we must end the write here
		if (first_index == FAT_EOC){
			return bytes_written;
		}

		first_index += sb->data_start_index;
		// printf("first index = %d\n", first_index);
		uint8_t bounce_buffer[BLOCK_SIZE];

		// bounce buffer contains the first block
		op_success = block_read(first_index, bounce_buffer);
		if (op_success == -1){
			// fprintf(stderr, "Error in fs_write(): failed to read first block, at index %d\n", first_index);
			return -1;
		}

		// printf("whole block\n");
		// printblock(bounce_buffer);

		// write_amount is the amount we are writing to this one block
		// the old implementation was under the assumption offset is less than one block
		// it was BLOCK_SIZE - offset, or the amount we are writing to the first block (partial block)
		// this should be fixed by changing offset to offset % BLOCK_SIZE
		uint16_t write_amount = 0;
		if ((offset % BLOCK_SIZE) + count < BLOCK_SIZE){
			write_amount = count;
		} else {
			write_amount = BLOCK_SIZE - (offset % BLOCK_SIZE);
		}

		// printf("write amount = %d\n", write_amount);
		// copy from the buffer starting from the offset
		// copy enough to fill the block, or the entire buffer if it is smaller
		memcpy(&bounce_buffer[offset % BLOCK_SIZE], buf, write_amount);
		// printf("bounce buffer after memcpy\n");
		// printblock(bounce_buffer);
		op_success = block_write(first_index, bounce_buffer);
		// uint8_t buffer[BLOCK_SIZE];
		// block_read(first_index, buffer);
		// printf("write result\n");
		// printblock(buffer);
		if (op_success == -1){
			// fprintf(stderr, "Error in fs_write(): failed to write first block, at index %d\n", first_index);
			return -1;
		}

		bytes_written += write_amount;
		full_block_start_index = (offset / BLOCK_SIZE) + 1;
	}
	
	// full blocks (if applicable)

	// num_full_blocks is the number of blocks we write to entirely
	int num_full_blocks = (count - (offset % BLOCK_SIZE)) / BLOCK_SIZE;
	for (int i = full_block_start_index; i < num_full_blocks + full_block_start_index; i++){
		uint16_t block_index = find_data_block(fd, i);

		// like for the partial block, if block_index == FAT_EOC, that means the request is out of bounds
		// this either means the loop ran for too long, or we were not able to allocate enough data blocks for the whole write
		// the first is an implementation error. lets hope it doesnt happen
		if (block_index == FAT_EOC){
			return bytes_written;
		}

		block_index += sb->data_start_index;

		// buf_offset is the part in buf we get the data from to write a block
		// if there was no offset, it should just be BLOCK_SIZE*i
		// if there was, it should be the amount of data already written
		// that would be (offset % BLOCK_SIZE) + (BLOCK_SIZE * (i-full_block_start_index))
		int buf_offset = (BLOCK_SIZE * (i-full_block_start_index)) + (offset % BLOCK_SIZE);

		op_success = block_write(block_index, buf+buf_offset);
		uint8_t buffer[BLOCK_SIZE];
		block_read(block_index, buffer);
		// printf("just after writing block\n");
		// printblock(buffer);

		if (op_success == -1){
			// fprintf(stderr, "Error in fs_write(): failed to write block %d, at index %d\n", i, block_index);
			return -1;
		}

		bytes_written += BLOCK_SIZE;
	}

	// the remainder block is the last partial block
	// if the last block we needed to write was a full block, we should skip this part
	int remainder_block_size = (count - (offset % BLOCK_SIZE)) % BLOCK_SIZE;
	// printf("remainder = %d\n", remainder_block_size);

	if (remainder_block_size != 0){
		uint16_t last_index = find_data_block(fd, num_target_blocks-1);

		// like above, last_index will be FAT_EOC if there is not enough space to write the last block
		// we must end the write here if so
		if (last_index == FAT_EOC){
			fprintf(stderr, "Error in fs_write(): no space to write last block");
			return bytes_written;
		}

		last_index += sb->data_start_index;
		// printf("last index = %d\n", last_index);
		uint8_t bounce_buffer[BLOCK_SIZE];

		// bounce buffer contains the last data block in the file
		op_success = block_read(last_index, bounce_buffer);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to read last block %d, at index %d\n", num_target_blocks-1, last_index);
			return -1;
		}

		// printf("whole block\n");
		// printblock(bounce_buffer);

		// buf_offset is the starting point for the data we are writing
		int buf_offset = BLOCK_SIZE*num_full_blocks + (offset % BLOCK_SIZE);
		// printf("buf offset = %d\n", buf_offset);
		// printf("index = %d\n", last_index);

		// overwrite buffer from 0 to remainder size with data
		memcpy(&bounce_buffer[0], buf+buf_offset, remainder_block_size);
		op_success = block_write(last_index, bounce_buffer);
		// printf("bounce buffer after write and memcpy\n");
		// printblock(bounce_buffer);
		// uint8_t buffer[BLOCK_SIZE];
		// block_read(last_index, buffer);
		// printf("write result\n");
		// printblock(buffer);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to write last block %d, at index %d\n", num_target_blocks-1, last_index);
			return -1;
		}

		bytes_written += remainder_block_size;
	}

	open_files[fd]->offset = bytes_written;
	open_files[fd]->file->file_size += bytes_written;
	return bytes_written;
}

int fs_read(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
	// printf("running fs_read\n");
	if (!is_disk_mounted){
		// fprintf(stderr, "Error in fs_read(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		// fprintf(stderr, "Error in fs_read(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		// fprintf(stderr, "Error in fs_read(): file with descriptor %d not open\n", fd);
		return -1;
	}

	if (buf == NULL){
		// fprintf(stderr, "Error in fs_read(): buf is null\n");
		return -1;
	}

	// uint8_t* byte_buf = (uint8_t*)buf;
	uint16_t offset = open_files[fd]->offset;
	int num_target_blocks = find_num_target_blocks(offset, count);
	int read_success = 0;
	int bytes_read = 0;
	uint16_t full_block_start_index = 0;
	// memcpy(dest, src, count)
	// printf("offset = %d\n", offset);
	// printf("full block start index = %d\n", full_block_start_index);

	// partial first block (if applicable)
	if (offset != 0){
		uint16_t first_index = find_data_block(fd, offset / BLOCK_SIZE);

		if (first_index == FAT_EOC){
			return bytes_read;
		}

		first_index += sb->data_start_index;
		uint8_t bounce_buffer[BLOCK_SIZE];
		read_success = block_read(first_index, bounce_buffer);
		if (read_success == -1){
			// fprintf(stderr, "Error in fs_read(): failed to read first block, at index %d\n", first_index);
			return -1;
		}

		// printf("whole block\n");
		// printblock(bounce_buffer);

		uint16_t read_amount = 0;
		if ((offset % BLOCK_SIZE) + count < BLOCK_SIZE){
			read_amount = count;
		} else {
			read_amount = BLOCK_SIZE - (offset % BLOCK_SIZE);
		}
		// printf("read amount = %d\n", read_amount);

		memcpy(buf, &bounce_buffer[offset % BLOCK_SIZE], read_amount);
		bytes_read += read_amount;
		full_block_start_index = (offset / BLOCK_SIZE) + 1;
	}

	// full blocks (if applicable)

	int num_full_blocks = (count - (offset % BLOCK_SIZE)) / BLOCK_SIZE;
	// printf("num_full_blocks = %d\n", num_full_blocks);
	for (int i = full_block_start_index; i < num_full_blocks + full_block_start_index; i++){
		uint16_t block_index = find_data_block(fd, i);

		if (block_index == FAT_EOC){
			return bytes_read;
		}

		block_index += sb->data_start_index;
		// printf("block index = %d\n", block_index);
		
		int buf_offset = (BLOCK_SIZE * (i-full_block_start_index)) + (offset % BLOCK_SIZE);

		// printf("buf offset = %d\n", buf_offset);

		read_success = block_read(block_index, buf+buf_offset);
		// uint8_t buffer[BLOCK_SIZE];
		// block_read(block_index, buffer);
		// printf("in read()\n");
		// printblock(buffer);

		if (read_success == -1){
			// fprintf(stderr, "Error in fs_read(): failed to read block %d, at index %d\n", i, block_index);
			return -1;
		}
		bytes_read += BLOCK_SIZE;
	}

	int remainder_block_size = (count - (offset % BLOCK_SIZE)) % BLOCK_SIZE;
	// printf("remainder = %d\n", remainder_block_size);
	// partial last block (if applicable)
	if (remainder_block_size != 0){
		uint16_t last_index = find_data_block(fd, num_target_blocks-1);

		if (last_index == FAT_EOC){
			return bytes_read;
		}

		last_index += sb->data_start_index;
		uint8_t bounce_buffer[BLOCK_SIZE];
		read_success = block_read(last_index, bounce_buffer);
		if (read_success == -1){
			// fprintf(stderr, "Error in fs_read(): failed to read last block %d, at index %d\n", num_target_blocks-1, last_index);
			return -1;
		}

		int buf_offset = BLOCK_SIZE*num_full_blocks + (offset % BLOCK_SIZE);
		memcpy(buf+buf_offset, &bounce_buffer[0], remainder_block_size);
		bytes_read += remainder_block_size;
	}

	open_files[fd]->offset = bytes_read;
	return bytes_read;
	// printf("%d %p %ld\n", fd, buf, count);
	// return 0;
}

