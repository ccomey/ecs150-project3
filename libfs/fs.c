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

struct FileDescriptor{
	struct File* file;
	uint16_t offset;
};

/* TODO: Phase 1 */

struct SuperBlock* sb;
uint16_t* fat;
struct Root* root;
struct FileDescriptor* open_files[FS_OPEN_MAX_COUNT];
uint8_t num_open_files = 0;
bool is_disk_mounted = false;

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
	fat = malloc(sb->num_data_blocks * sizeof(uint16_t));
	if (fat == NULL){
		fprintf(stderr, "Error in load_fat(): malloc failed\n");
		return -1;
	}

	uint16_t buf_size;
	if (sb->num_data_blocks*sizeof(uint16_t) > BLOCK_SIZE){
		buf_size = sb->num_data_blocks * sizeof(uint16_t);
	} else {
		buf_size = BLOCK_SIZE;
	}

	// printf("buf size = %d\n", buf_size);
	// printf("num fat blocks = %d\n", sb->num_fat_blocks);

	uint16_t buffer[buf_size];
	int read_success;

	for (unsigned i = 0; i < sb->num_fat_blocks; i++){
		// printf("i+1=%d\n", i+1);
		// printf("BLOCK_SIZE * %d = %d\n", i, BLOCK_SIZE*i);
		read_success = block_read(i+1, &buffer[BLOCK_SIZE*i]);
		if (read_success == -1){
			fprintf(stderr, "Error in load_fat: failed to read block at index %d\n", i+1);
			return -1;
		}
	}

	// for (uint16_t i = 0; i < sb->num_data_blocks; i++){
	// 	printf("%d ", buffer[i]);
	// }
	// printf("\n\n");
	for (uint16_t i = 0; i < sb->num_data_blocks; i++){
		*(fat+i) = buffer[i];
		// printf("%d\n", *(fat+i));
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
		// printf("in load_root_directory: %d\n", root->files[f]->first_index);
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
	// printf("running allocate_blocks_in_fat\n");
	uint16_t block_index = open_files[fd]->file->first_index;
	int num_data_blocks = 0;

	// if our first index is FAT_EOC, then the file has no data blocks allocated
	// so we iterate through the FAT until we find an empty entry, then make that the file's first block
	if (block_index == FAT_EOC){
		for (int i = 1; i < sb->num_data_blocks; i++){
			if (*(fat+i) == 0){
				*(fat+i) = FAT_EOC;
				block_index = i;
				open_files[fd]->file->first_index = block_index;
				num_data_blocks = 1;
				break;
			}
		}
	}

	// we then follow the chain of data blocks for the file until we reach the end
	for (block_index = open_files[fd]->file->first_index; *(fat+block_index) != FAT_EOC; num_data_blocks++){
		block_index = *(fat+block_index);
	}


	// now we extend the chain out to our desired length
	while (num_data_blocks < num_target_blocks){
		int found_new_index = 0;
		// we start at 1 because fat[0] is always invalid
		for (int i = 1; i < sb->num_data_blocks; i++){
			// 0 signifies an empty entry available to incorporate into our file's linked list
			if (*(fat+i) == 0){
				printf("block index = %d\n", block_index);
				*(fat+block_index) = i;
				*(fat+i) = FAT_EOC;
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
	// printf("first index = %d\n", first_index);
	if (block_num == 0){
		return first_index;
	}

	uint16_t block_index = first_index;
	for (int i = 0; i < block_num; i++){
		if (block_index == FAT_EOC){
			fprintf(stderr, "Error in find_data_block(): request is out of bounds for the file\n");
			return -1;
		}
		block_index = *(fat+block_index);
		// printf("%d : %d\n", i, block_index);
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

	// printf("first index as read by unmount: %d\n", root->files[0]->first_index);
	// uint8_t buffer[BLOCK_SIZE];
	// block_read(root->files[0]->first_index, buffer);
	// printblock(buffer);

	for (uint16_t i = 0; i < sb->num_fat_blocks; i++){
		block_write(i+1, fat+BLOCK_SIZE*i);
	}

	uint8_t root_array[BLOCK_SIZE];
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		int offset = 32*i;
		struct File* file = root->files[i];
		for (unsigned j = 0; j < FS_FILENAME_LEN; j++){
			root_array[offset+j] = file->filename[j];
		}

		offset += FS_FILENAME_LEN;
		// file size is 4 bytes
		root_array[offset+3] = (file->file_size >> 24) & 0xFF;
		root_array[offset+2] = (file->file_size >> 16) & 0xFF;
		root_array[offset+1] = (file->file_size >> 8) & 0xFF;
		root_array[offset+0] = (file->file_size >> 0) & 0xFF;

		offset += 4;

		root_array[offset+1] = (file->first_index >> 8) & 0xFF;
		root_array[offset+0] = (file->first_index >> 0) & 0xFF;

		offset += 2;

		for (unsigned j = 0; j < 10; j++){
			root_array[offset+j] = file->padding[j];
		}
	}

	// for (uint8_t i = 0; i < FS_FILE_MAX_COUNT; i++){
	// 	for (uint8_t j = 0; j < 32; j++){
	// 		if (j < FS_FILENAME_LEN){
	// 			printf("%c", root_array[32*i+j]);
	// 		} else {
	// 			printf(" %d", root_array[32*i+j]);
	// 		}
	// 	}
	// 	printf("\n");
	// }

	// for (uint8_t i = 0; i < FS_FILE_MAX_COUNT; i++){
	// 	printf("%d\n", root->files[i]->first_index);
	// }

	block_write(sb->root_index, root_array);

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
	// printf("running info\n");
	if (!is_disk_mounted){
		fprintf(stderr, "Error in fs_info(): no disk is mounted\n");
		return -1;
	}

	printf("FS Info:\n");
	printf("total_blk_count=%d\n", block_disk_count());
	printf("fat_blk_count=%d\n", sb->num_fat_blocks);
	printf("rdir_blk=%d\n", sb->root_index);
	printf("data_blk=%d\n", sb->data_start_index);
	printf("data_blk_count=%d\n", sb->num_data_blocks);

	int free_fat_entries = 0;
	for (uint16_t i = 0; i < sb->num_data_blocks; i++){
		if (*(fat+i) == 0){
			free_fat_entries++;
		}
	}

	printf("fat_free_ratio=%d/%d\n", free_fat_entries, sb->num_data_blocks);

	int free_root_entries = 0;
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		if (root->files[i]->filename[0] == '\0'){
			free_root_entries++;
		} else {

			// uint8_t buffer[BLOCK_SIZE];
			// block_read(root->files[i]->first_index, buffer);
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
	// printf("in fs_create: %d\n", root->files[empty_index]->first_index);

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
		if (open_files[i] != NULL && streq(open_files[i]->file->filename, (uint8_t*)filename) == 0){
			fprintf(stderr, "Error in fs_delete(): open files cannot be deleted\n");
			return -1;
		}
	}

	struct File* old_file = root->files[matching_file_index];

	uint16_t block_index = old_file->first_index;
	while (*(fat+block_index) != FAT_EOC){
		uint16_t old_block_index = block_index;
		*(fat+old_block_index) = 0;
		block_index = *(fat+block_index);
	}

	*(fat+block_index) = 0;

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

	printf("FS Ls:\n");
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		struct File* file = root->files[i];
		if (file->filename[0] != 0){
			// printf("File #%d\n", i);
			// printf("Name: %s\n", root->files[i]->filename);
			// printf("Size: %d\n", root->files[i]->file_size);
			// printf("First index: %d\n", root->files[i]->first_index);
			printf("file: %s, size: %d, data_blk: %d\n", file->filename, file->file_size, file->first_index);
		}
	}
	return 0;
}

int fs_open(const char *filename)
{
	/* TODO: Phase 3 */
	// printf("running open\n");
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
	// printf("running close\n");
	// printf("first index as read by fs close: %d\n", open_files[fd]->file->first_index);
	// uint8_t buffer[BLOCK_SIZE];
	// block_read(open_files[fd]->file->first_index, buffer);
	// printblock(buffer);

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
	// printf("running fs_write\n");

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

	if (count == 0){
		return 0;
	}

	uint16_t offset = open_files[fd]->offset;
	int op_success = 0;
	int bytes_written = 0;
	uint16_t full_block_start_index = 0;
	int num_target_blocks = find_num_target_blocks(offset, count);
	// printf("write\noffset = %d\n", offset);
	// printf("count = %ld\n", count);
	
	op_success = allocate_blocks_in_fat(fd, num_target_blocks);
	if (op_success == -1){
		fprintf(stderr, "Error in fs_write(): block allocation failed\n");
		return -1;
	}

	// partial first block (if applicable)
	// printf("offset = %d\n", offset);
	if (offset != 0){
		uint16_t first_index = open_files[fd]->file->first_index;
		// printf("first index = %d\n", sb->data_start_index + first_index);
		uint8_t bounce_buffer[BLOCK_SIZE];

		// bounce buffer contains the first block
		op_success = block_read(sb->data_start_index + first_index, bounce_buffer);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to read first block, at index %d\n", first_index);
			return -1;
		}

		// printf("whole block\n");
		// printblock(bounce_buffer);

		uint16_t write_amount = 0;
		if (offset + count < BLOCK_SIZE){
			write_amount = count;
		} else {
			write_amount = BLOCK_SIZE - offset;
		}

		// printf("write amount = %d\n", write_amount);
		// copy from the buffer starting from the offset
		// copy enough to fill the block, or the entire buffer if it is smaller
		memcpy(&bounce_buffer[offset], buf, write_amount);
		// printf("bounce buffer after memcpy\n");
		// printblock(bounce_buffer);
		op_success = block_write(sb->data_start_index + first_index, bounce_buffer);
		// uint8_t buffer[BLOCK_SIZE];
		// block_read(sb->data_start_index + first_index, buffer);
		// printf("write result\n");
		// printblock(buffer);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to write first block, at index %d\n", sb->data_start_index + first_index);
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

		op_success = block_write(sb->data_start_index + block_index, buf+buf_offset);
		uint8_t buffer[BLOCK_SIZE];
		block_read(sb->data_start_index + block_index, buffer);
		// printf("just after writing block\n");
		printblock(buffer);

		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to write block %d, at index %d\n", i, sb->data_start_index + block_index);
			return -1;
		}

		bytes_written += BLOCK_SIZE;
	}

	int remainder_block_size = (count-offset) % BLOCK_SIZE;
	// printf("remainder = %d\n", remainder_block_size);

	if (remainder_block_size != 0){
		uint16_t last_index = find_data_block(fd, num_target_blocks-1);
		// printf("last index = %d\n", last_index);
		uint8_t bounce_buffer[BLOCK_SIZE];

		// bounce buffer contains the last data block in the file
		op_success = block_read(sb->data_start_index + last_index, bounce_buffer);
		if (op_success == -1){
			fprintf(stderr, "Error in fs_write(): failed to read last block %d, at index %d\n", num_target_blocks-1, last_index);
			return -1;
		}

		// printf("whole block\n");
		// printblock(bounce_buffer);

		int buf_offset = BLOCK_SIZE*num_full_blocks + offset;
		// printf("buf offset = %d\n", buf_offset);
		// printf("index = %d\n", sb->data_start_index+last_index);

		// overwrite buffer from 0 to remainder size with data
		memcpy(&bounce_buffer[0], buf+buf_offset, remainder_block_size);
		op_success = block_write(sb->data_start_index + last_index, bounce_buffer);
		// printf("bounce buffer after write and memcpy\n");
		// printblock(bounce_buffer);
		// uint8_t buffer[BLOCK_SIZE];
		// block_read(sb->data_start_index + last_index, buffer);
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
		uint16_t first_index = open_files[fd]->file->first_index;
		uint8_t bounce_buffer[BLOCK_SIZE];
		read_success = block_read(sb->data_start_index + first_index, bounce_buffer);
		if (read_success == -1){
			fprintf(stderr, "Error in fs_read(): failed to read first block, at index %d\n", sb->data_start_index + first_index);
			return -1;
		}

		// printf("whole block\n");
		// printblock(bounce_buffer);

		uint16_t read_amount = 0;
		if (offset + count < BLOCK_SIZE){
			read_amount = count;
		} else {
			read_amount = BLOCK_SIZE - offset;
		}
		// printf("read amount = %d\n", read_amount);

		memcpy(buf, &bounce_buffer[offset], read_amount);
		bytes_read += read_amount;
		full_block_start_index = 1;
	}

	// full blocks (if applicable)

	int num_full_blocks = (count-offset) / BLOCK_SIZE;
	// printf("num_full_blocks = %d\n", num_full_blocks);
	for (int i = full_block_start_index; i < num_full_blocks + full_block_start_index; i++){
		uint16_t block_index = find_data_block(fd, i);
		// printf("block index = %d\n", block_index);
		int buf_offset = 0;
		if (offset == 0){
			buf_offset = BLOCK_SIZE*i;
		} else {
			buf_offset = BLOCK_SIZE*(i-1) + offset;
		}

		// printf("buf offset = %d\n", buf_offset);

		read_success = block_read(sb->data_start_index + block_index, buf+buf_offset);
		// uint8_t buffer[BLOCK_SIZE];
		// block_read(block_index, buffer);
		// printf("in read()\n");
		// printblock(buffer);

		if (read_success == -1){
			fprintf(stderr, "Error in fs_read(): failed to read block %d, at index %d\n", i, sb->data_start_index + block_index);
			return -1;
		}
		bytes_read += BLOCK_SIZE;
	}

	int remainder_block_size = (count-offset) % BLOCK_SIZE;
	// printf("remainder = %d\n", remainder_block_size);
	// partial last block (if applicable)
	if (remainder_block_size != 0){
		uint16_t last_index = find_data_block(fd, num_target_blocks-1);
		uint8_t bounce_buffer[BLOCK_SIZE];
		read_success = block_read(sb->data_start_index + last_index, bounce_buffer);
		if (read_success == -1){
			fprintf(stderr, "Error in fs_read(): failed to read last block %d, at index %d\n", num_target_blocks-1, sb->data_start_index + last_index);
			return -1;
		}

		int buf_offset = BLOCK_SIZE*num_full_blocks + offset;
		memcpy(buf+buf_offset, &bounce_buffer[0], remainder_block_size);
		bytes_read += remainder_block_size;
	}

	open_files[fd]->offset = bytes_read;
	return bytes_read;
	// printf("%d %p %ld\n", fd, buf, count);
	// return 0;
}

