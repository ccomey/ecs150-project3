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

#define FATBLOCK_FILENO 2048
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
	uint8_t padding[10];
};

struct Root{
	struct File* files[FS_FILE_MAX_COUNT];
};

struct FAT{
	uint16_t indices[FATBLOCK_FILENO];
};

struct FileDescriptor{
	struct File* file;
	uint16_t offset;
};

/* TODO: Phase 1 */

struct SuperBlock* sb;
struct FAT* fat;
struct Root* root;
struct FileDescriptor* open_files[FS_OPEN_MAX_COUNT];
uint8_t num_open_files = 0;
bool is_disk_mounted = false;

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

uint16_t concatenate_two_bytes(uint8_t byte1, uint8_t byte2){
	uint16_t result = 0;
	result += 256*byte2;
	result += byte1;
	return result;
}

uint32_t concatenate_four_bytes(uint8_t byte1, uint8_t byte2, uint8_t byte3, uint8_t byte4){
	uint32_t result = 0;
	result += pow(2, 24)*byte4;
	result += pow(2, 16)*byte3;
	result += pow(2, 8)*byte2;
	result += byte1;
	return result;
}

bool is_filename_invalid(const char* filename){
	return filename == NULL || *filename == '\0' || strlen(filename)+1 > FS_FILENAME_LEN;
}

int load_disk(const char* diskname){
	if (is_disk_mounted){
		fprintf(stderr, "Error in fs_mount(): disk already mounted\n");
		return -1;
	}
	int open_disk_success = block_disk_open(diskname);
	// printf("disk success = %d\n", open_disk_success);
	if (open_disk_success != 0){
		fprintf(stderr, "Error in fs_mount(): could not read superblock\n");
		return -1;
	}

	is_disk_mounted = true;
	return 0;
}

int load_superblock(uint8_t* superblock_ptr){
	sb = malloc(sizeof(struct SuperBlock));
	if (superblock_ptr == NULL){
		fprintf(stderr, "Error in fs_mount(): pointer to superblock is null\n");
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
		fprintf(stderr, "Error in fs_mount(): num_blocks read from superblock does not match number on disk\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb->root_index = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb->root_index <= 0){
		fprintf(stderr, "Error in fs_mount(): root index is where superblock should be\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb->data_start_index = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb->data_start_index == 0){
		fprintf(stderr, "Error in fs_mount(): data start index is where superblock should be\n");
		return -1;
	}

	if (sb->data_start_index == sb->root_index){
		fprintf(stderr, "Error in fs_mount(): root and data start have the same index\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb->num_data_blocks = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb->num_data_blocks >= sb->num_blocks){
		fprintf(stderr, "Error in fs_mount(): more data blocks than total blocks\n");
		return -1;
	}

	// printf("%d\n", *superblock_ptr);
	sb->num_fat_blocks = *(superblock_ptr);
	superblock_ptr++;

	if (sb->num_fat_blocks >= sb->num_blocks){
		fprintf(stderr, "Error in fs_mount(): more FAT blocks than total blocks\n");
		return -1;
	}

	for (unsigned i = SUPERBLOCK_PREPAD_LEN; i < BLOCK_SIZE; i++){
		sb->padding[i] = *(superblock_ptr++);

		if (sb->padding[i] != 0){
			fprintf(stderr, "Error in fs_mount(): incorrect padding formatting\n");
			return -1;
		}
	}

	return 0;
}

int load_fat(){
	fat = malloc(sizeof(struct FAT));
	if (fat == NULL){
		fprintf(stderr, "Error in load_fat_blocks(): malloc failed\n");
		return -1;
	}

	uint16_t buffer[sb->num_fat_blocks*FATBLOCK_FILENO];
	int read_success;

	for (unsigned i = 0; i < sb->num_fat_blocks; i++){
		read_success = block_read(i+1, &buffer[FATBLOCK_FILENO*i]);
		if (read_success == -1){
			fprintf(stderr, "Error in load_fat_blocks: failed to read block at index %d\n", i+1);
			return -1;
		}
	}

	for (uint16_t index = 0; index < sb->num_fat_blocks*FATBLOCK_FILENO/sizeof(index); index++){
		fat->indices[index] = buffer[index];
	}

	return 0;
}

int load_root_directory(uint8_t* root_ptr){
	// printf("root dir\n");
	// for (unsigned i = 0; i < BLOCK_SIZE; i++){
	// 	printf("%d ", *(root_ptr+i));
	// }
	// printf("\n");

	root = malloc(sizeof(struct Root));
	for (unsigned f = 0; f < FS_FILE_MAX_COUNT; f++){
		struct File* file = malloc(sizeof(struct File));
		for (unsigned i = 0; i < FS_FILENAME_LEN; i++){
			file->filename[i] = *(root_ptr++);
		}

		file->file_size = concatenate_four_bytes(*root_ptr, *(root_ptr+1), *(root_ptr+2), *(root_ptr+3));
		root_ptr += 4;

		file->first_index = concatenate_two_bytes(*root_ptr, *(root_ptr+1));
		root_ptr += 2;

		for (unsigned i = 0; i < 10; i++){
			file->padding[i] = (*root_ptr++);
		}

		root->files[f] = file;
	}

	return 0;
}

// finds the first empty entry in the root directory and returns its index
// returns -1 if unable to find empty entry
int find_empty_root_entry(){
	for (unsigned i = 0; i < FS_FILENAME_LEN; i++){
		if (root->files[i]->filename[0] == 0){
			return i;
		}
	}

	return -1;
}

// returns -1 if there is no matching filename in the root directory
// returns the file index if there is a matching filename
// no error checking here because that is done before it is called
int find_matching_filename(const char* filename){
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		if (strcmp(filename, (char*)(root->files[i]->filename)) == 0){
			return i;
		}
	}

	return -1;
}

int add_file_to_fd_array(struct File* file){
	if (num_open_files > FS_OPEN_MAX_COUNT){
		return -1;
	}
	
	struct FileDescriptor* fd = malloc(sizeof(struct FileDescriptor));
	fd->file = file;
	fd->offset = 0;
	
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

int find_num_target_blocks(uint16_t offset, size_t count){
	if (offset + count < BLOCK_SIZE){
		return 1;
	}

	int num_target_blocks = 1;
	if (offset != 0){
		count -= offset;
		num_target_blocks++;
	}

	num_target_blocks += count / BLOCK_SIZE;

	if (count % BLOCK_SIZE != 0){
		num_target_blocks++;
	}

	return num_target_blocks;
}

int allocate_blocks_in_fat(const int fd, const int num_target_blocks){
	uint16_t block_index = open_files[fd]->file->first_index;
	int num_data_blocks = 0;

	if (block_index == FAT_EOC){
		for (int i = 1; i < FATBLOCK_FILENO; i++){
			if (fat->indices[i] == 0){
				fat->indices[i] = FAT_EOC;
				block_index = i;
				open_files[fd]->file->first_index = block_index;
				num_data_blocks = 1;
				break;
			}
		}
	}

	for (block_index = open_files[fd]->file->first_index; block_index != FAT_EOC; num_data_blocks++){
		block_index = fat->indices[block_index];
	}

	while (num_data_blocks < num_target_blocks){
		int found_new_index = 0;
		// we start at 1 because fat[0] is always invalid
		for (int i = 1; i < FATBLOCK_FILENO; i++){
			// 0 signifies an empty entry available to incorporate into our file's linked list
			if (fat->indices[i] == 0){
				fat->indices[block_index] = i;
				fat->indices[i] = FAT_EOC;
				num_data_blocks++;
				block_index = i;
				found_new_index = 1;
				break;
			}
		}

		if (!found_new_index){
			fprintf(stderr, "Error in allocate_blocks_in_fat(): unable to find empty FAT entry\n");
			return -1;
		}
	}

	return 0;
}

int find_data_block(const int fd, const int block_num){
	uint16_t first_index = open_files[fd]->file->first_index;
	if (block_num == 0){
		return first_index;
	}

	uint16_t block_index = first_index;
	for (int i = 0; i < block_num; i++){
		if (block_index == FAT_EOC){
			fprintf(stderr, "Error in find_data_block(): request is out of bounds for the file\n");
			return -1;
		}
		block_index = fat->indices[block_index];
	}

	return block_index;
}

int fs_mount(const char *diskname)
{
	/* TODO: Phase 1 */
	// printf("starting fs_mount\n");

	// open the disk
	if (load_disk(diskname) != 0){
		fprintf(stderr, "Error in fs_mount(): could not open disk\n");
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
;	if (load_root_directory(root_ptr) != 0){
		return -1;
	}

	return 0;
}

int fs_umount(void)
{
	/* TODO: Phase 1 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_unmount(): no disk is mounted\n");
		return -1;
	}

	if (num_open_files > 0){
		fprintf(stderr, "Error in fs_unmount(): cannot unmount until all files are closed\n");
	}
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		free(root->files[i]);
	}

	free(root);
	free(fat);
	free(sb);

	block_disk_close();

	is_disk_mounted = false;
	return 0;
}

int fs_info(void)
{
	/* TODO: Phase 1 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_info(): no disk is mounted\n");
		return -1;
	}

	printf("Signature: ");
	for (unsigned i = 0; i < 8; i++){
		printf("%c", sb->signature[i]);
	}

	printf("\n");

	printf("Number of blocks: %d\n", sb->num_blocks);
	printf("Root index: %d\n", sb->root_index);
	printf("Data start index: %d\n", sb->data_start_index);
	printf("Number of data blocks: %d\n", sb->num_data_blocks);
	printf("Number of FAT blocks: %d\n", sb->num_fat_blocks);
	
	// printf("Padding: ");
	// for (unsigned i = 0; i < SUPERBLOCK_PAD_LEN; i++){
	// 	printf("%d", sb->padding[i]);
	// }

	// printf("\n");

	printf("Number of blocks according to block_disk_count: %d\n", block_disk_count());
	
	printf("FAT Blocks\n");
	for (unsigned i = 0; i < sb->num_fat_blocks; i++){
		printf("FAT Block #%d\n", i);
		for (unsigned j = 0; j < FATBLOCK_FILENO; j++){
			printf("%d ", fat->indices[i*FATBLOCK_FILENO + j]);
		}
		printf("\n");
	}

	printf("\n");

	printf("Root directory\n");
	for (unsigned f = 0; f < FS_FILE_MAX_COUNT; f++){
		printf("File #%d\n", f);
		printf("Name: %s\n", root->files[f]->filename);
		printf("Size: %d\n", root->files[f]->file_size);
		printf("First index: %d\n", root->files[f]->first_index);
		printf("Padding: ");
		for (unsigned i = 0; i < 10; i++){
			printf("%d ", root->files[f]->padding[i]);
		}

		printf("\n");
	}

	printf("\n");
	
	return 0;
}

int fs_create(const char *filename)
{
	/* TODO: Phase 2 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_create(): no disk is mounted\n");
		return -1;
	}
	if (is_filename_invalid(filename)){
		fprintf(stderr, "Error in fs_create(): invalid file name\n");
		return -1;
	}

	if (find_matching_filename(filename) != -1){
		fprintf(stderr, "Error in fs_create(): file named %s already exists\n", filename);
		return -1;
	}

	// find new entry
	int empty_index = find_empty_root_entry();
	if (empty_index == -1){
		fprintf(stderr, "Error in fs_create(): unable to find empty index\n");
		return -1;
	}
	struct File* new_file = root->files[empty_index];

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

	for (unsigned i = 0; i < FS_OPEN_MAX_COUNT; i++){
		open_files[i] = NULL;
	}

	// printf("%s\n", filename);
	return 0;
}

int fs_delete(const char *filename)
{
	/* TODO: Phase 2 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_delete(): no disk is mounted\n");
		return -1;
	}

	if (is_filename_invalid(filename)){
		fprintf(stderr, "Error in fs_delete(): invalid file name\n");
		return -1;
	}

	int matching_file_index = find_matching_filename(filename);
	if (matching_file_index == -1){
		fprintf(stderr, "Error in fs_create(): file named %s does not exist\n", filename);
		return -1;
	}

	for (unsigned i = 0; i < FS_OPEN_MAX_COUNT; i++){
		if (streq(open_files[i]->file->filename, (uint8_t*)filename) == 0){
			fprintf(stderr, "Error in fs_delete(): open files cannot be deleted\n");
			return -1;
		}
	}

	struct File* old_file = root->files[matching_file_index];

	uint16_t block_index = old_file->first_index;
	while (fat->indices[block_index] != FAT_EOC){
		uint16_t old_block_index = block_index;
		fat->indices[old_block_index] = 0;
		block_index = fat->indices[block_index];
	}

	fat->indices[block_index] = 0;

	old_file->filename[0] = '\0';
	old_file->file_size = 0;
	old_file->first_index = FAT_EOC;

	return 0;
}

int fs_ls(void)
{
	/* TODO: Phase 2 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_ls(): no disk is mounted\n");
	}
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		if (root->files[i]->filename[0] != 0){
			printf("File #%d\n", i);
			printf("Name: %s\n", root->files[i]->filename);
			printf("Size: %d\n", root->files[i]->file_size);
			printf("First index: %d\n", root->files[i]->first_index);
		}
	}
	return 0;
}

int fs_open(const char *filename)
{
	/* TODO: Phase 3 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_open(): no disk is mounted\n");
		return -1;
	}

	if (is_filename_invalid(filename)){
		fprintf(stderr, "Error in fs_create(): invalid file name\n");
		return -1;
	}

	int file_index = find_matching_filename(filename);
	if (file_index == -1){
		fprintf(stderr, "Error in fs_open(): file %s not found\n", filename);
		return -1;
	}

	int fd = add_file_to_fd_array(root->files[file_index]);
	if (fd == -1){
		fprintf(stderr, "Error in fs_open(): max number of files already open\n");
		return -1;
	}

	return fd;
}

int fs_close(int fd)
{
	/* TODO: Phase 3 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_close(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		fprintf(stderr, "Error in fs_close(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		fprintf(stderr, "Error in fs_close(): file with descriptor %d not open\n", fd);
		return -1;
	}

	open_files[fd] = NULL;
	num_open_files--;
	return 0;
}

int fs_stat(int fd)
{
	/* TODO: Phase 3 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_stat(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		fprintf(stderr, "Error in fs_stat(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		fprintf(stderr, "Error in fs_stat(): file with descriptor %d not open\n", fd);
		return -1;
	}

	return open_files[fd]->file->file_size;
}

int fs_lseek(int fd, size_t offset)
{
	/* TODO: Phase 3 */
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_lseek(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		fprintf(stderr, "Error in fs_lseek(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		fprintf(stderr, "Error in fs_lseek(): file with descriptor %d not open\n", fd);
		return -1;
	}

	if (offset > open_files[fd]->file->file_size){
		fprintf(stderr, "Error in fs_lseek(): specified offset too large (%ld vs %d)\n", offset, open_files[fd]->file->file_size);
		return -1;
	}

	open_files[fd]->offset = offset;
	return 0;
}

int fs_write(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
	/**
	 * fs_write - Write to a file
	 * @fd: File descriptor
	 * @buf: Data buffer to write in the file
	 * @count: Number of bytes of data to be written
	 *
	 * Attempt to write @count bytes of data from buffer pointer by @buf into the
	 * file referenced by file descriptor @fd. It is assumed that @buf holds at
	 * least @count bytes.
	 *
	 * When the function attempts to write past the end of the file, the file is
	 * automatically extended to hold the additional bytes. If the underlying disk
	 * runs out of space while performing a write operation, fs_write() should write
	 * as many bytes as possible. The number of written bytes can therefore be
	 * smaller than @count (it can even be 0 if there is no more space on disk).
	 * 
	 * Return: -1 if no FS is currently mounted, or if file descriptor @fd is
	 * invalid (out of bounds or not currently open), or if @buf is NULL. Otherwise
	 * return the number of bytes actually written.
	*/

	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_write(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		fprintf(stderr, "Error in fs_write(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		fprintf(stderr, "Error in fs_write(): file with descriptor %d not open\n", fd);
		return -1;
	}

	if (buf == NULL){
		fprintf(stderr, "Error in fs_write(): buf is null\n");
		return -1;
	}

	uint8_t* byte_buf = (uint8_t*)buf;
	uint16_t offset = open_files[fd]->offset;
	int op_success = 0;
	int bytes_written = 0;
	uint16_t full_block_start_index = 0;
	int num_target_blocks = find_num_target_blocks(offset, count);
	
	op_success = allocate_blocks_in_fat(fd, num_target_blocks);
	if (op_success == -1){
		fprintf(stderr, "Error in fs_write(): block allocation failed\n");
		return -1;
	}

	// partial first block (if applicable)
	if (offset != 0){
		uint16_t first_index = open_files[fd]->file->first_index;
		uint8_t bounce_buffer[BLOCK_SIZE];

		// bounce buffer contains the first block
		op_success = block_read(first_index, bounce_buffer);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to read first block, at index %d\n", first_index);
			return -1;
		}

		uint16_t write_amount = 0;
		if (offset + count < BLOCK_SIZE){
			write_amount = count;
		} else {
			write_amount = BLOCK_SIZE - offset;
		}
		// copy from the buffer starting from the offset
		// copy enough to fill the block, or the entire buffer if it is smaller
		memcpy(&bounce_buffer[offset], byte_buf, write_amount);
		op_success = block_write(first_index, bounce_buffer);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to write first block, at index %d\n", first_index);
			return -1;
		}

		bytes_written += write_amount;
		full_block_start_index = 1;
	}
	
	// full blocks (if applicable)

	int num_full_blocks = (count-offset) / BLOCK_SIZE;
	for (int i = full_block_start_index; i < num_full_blocks + full_block_start_index; i++){
		uint16_t block_index = find_data_block(fd, i);
		int buf_offset = (offset == 0) ? (BLOCK_SIZE*i) : (BLOCK_SIZE*(i-1)+offset);

		op_success = block_write(block_index, buf+buf_offset);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to write block %d, at index %d\n", i, block_index);
			return -1;
		}

		bytes_written += BLOCK_SIZE;
	}

	int remainder_block_size = (count-offset) % BLOCK_SIZE;

	if (remainder_block_size != 0){
		uint16_t last_index = find_data_block(fd, num_target_blocks-1);
		uint8_t bounce_buffer[BLOCK_SIZE];

		// bounce buffer contains the last data block in the file
		op_success = block_read(last_index, bounce_buffer);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to read last block %d, at index %d\n", num_target_blocks-1, last_index);
			return -1;
		}

		int buf_offset = BLOCK_SIZE*num_full_blocks + offset;

		// overwrite buffer from 0 to remainder size with data
		memcpy(&bounce_buffer[0], byte_buf+buf_offset, remainder_block_size);
		op_success = block_write(last_index, bounce_buffer);
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
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_read(): no disk is mounted\n");
		return -1;
	}

	if (fd < 0 || fd >= FS_FILE_MAX_COUNT){
		fprintf(stderr, "Error in fs_read(): file descriptor %d out of bounds\n", fd);
		return -1;
	}

	if (open_files[fd] == NULL){
		fprintf(stderr, "Error in fs_read(): file with descriptor %d not open\n", fd);
		return -1;
	}

	if (buf == NULL){
		fprintf(stderr, "Error in fs_read(): buf is null\n");
		return -1;
	}

	uint8_t* byte_buf = (uint8_t*)buf;
	uint16_t offset = open_files[fd]->offset;
	int num_target_blocks = find_num_target_blocks(offset, count);
	int read_success = 0;
	int bytes_read = 0;
	uint16_t full_block_start_index = 0;
	// memcpy(dest, src, count)

	// partial first block (if applicable)
	if (offset != 0){
		uint16_t first_index = open_files[fd]->file->first_index;
		uint8_t bounce_buffer[BLOCK_SIZE];
		read_success = block_read(first_index, bounce_buffer);
		if (read_success == -1){
			fprintf(stderr, "Error in fs_read(): failed to read first block, at index %d\n", first_index);
			return -1;
		}

		uint16_t read_amount = 0;
		if (offset + count < BLOCK_SIZE){
			read_amount = count;
		} else {
			read_amount = BLOCK_SIZE - offset;
		}

		memcpy(byte_buf, &bounce_buffer[offset], read_amount);
		bytes_read += read_amount;
		full_block_start_index = 1;
	}

	// full blocks (if applicable)

	int num_full_blocks = (count-offset) / BLOCK_SIZE;
	for (int i = full_block_start_index; i < num_full_blocks + full_block_start_index; i++){
		uint16_t block_index = find_data_block(fd, i);
		int buf_offset = 0;
		if (offset == 0){
			buf_offset = BLOCK_SIZE*i;
		} else {
			buf_offset = BLOCK_SIZE*(i-1) + offset;
		}

		read_success = block_read(block_index, byte_buf+buf_offset);
		if (read_success == -1){
			fprintf(stderr, "Error in fs_read(): failed to read block %d, at index %d\n", i, block_index);
			return -1;
		}
		bytes_read += BLOCK_SIZE;
	}

	int remainder_block_size = (count-offset) % BLOCK_SIZE;
	// partial last block (if applicable)
	if (remainder_block_size != 0){
		uint16_t last_index = find_data_block(fd, num_target_blocks-1);
		uint8_t bounce_buffer[BLOCK_SIZE];
		read_success = block_read(last_index, bounce_buffer);
		if (read_success == -1){
			fprintf(stderr, "Error in fs_read(): failed to read last block %d, at index %d\n", num_target_blocks-1, last_index);
			return -1;
		}

		int buf_offset = BLOCK_SIZE*num_full_blocks + offset;
		memcpy(byte_buf+buf_offset, &bounce_buffer[0], remainder_block_size);
		bytes_read += remainder_block_size;
	}

	open_files[fd]->offset = bytes_read;
	return bytes_read;
	// printf("%d %p %ld\n", fd, buf, count);
	// return 0;
}

