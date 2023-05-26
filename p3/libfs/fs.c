#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "disk.h"
#include "fs.h"

#define SUPERBLOCK_SIG_LEN 8
#define SUPERBLOCK_PREPAD_LEN SUPERBLOCK_SIG_LEN+2+2+2+2+1
#define SUPERBLOCK_PAD_LEN BLOCK_SIZE-SUPERBLOCK_PREPAD_LEN

#define FATBLOCK_FILENO 2048
#define FAT_EOC 0xFFFF

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

struct FATBlock{
	uint16_t files[FATBLOCK_FILENO];
};

/* TODO: Phase 1 */

struct SuperBlock* sb;
struct FATBlock** fat_blocks;
struct Root* root;

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

int load_disk(const char* diskname){
	int open_disk_success = block_disk_open(diskname);
	// printf("disk success = %d\n", open_disk_success);
	if (open_disk_success != 0){
		fprintf(stderr, "Error in fs_mount(): could not read superblock\n");
		return -1;
	}

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

int load_fat_blocks(uint8_t* fat_block_ptr){
	fat_blocks = malloc(sizeof(fat_blocks));
	if (fat_blocks == NULL){
		return -1;
	}

	for (unsigned i = 0; i < sb->num_fat_blocks; i++){
		// printf("allocating fat block #%d\n", i);
		struct FATBlock* fat_block = malloc(sizeof(struct FATBlock));
		if (fat_block == NULL){
			return -1;
		}
		for (unsigned j = 0; j < FATBLOCK_FILENO; j++){
			fat_block->files[j] = concatenate_two_bytes(*fat_block_ptr, *(fat_block_ptr+1));
			fat_block_ptr += 2;
		}

		fat_blocks[i] = fat_block;
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
	uint8_t superblock_buffer[BLOCK_SIZE];
	void* buf_ptr = &superblock_buffer;

	int read_success = block_read(0, buf_ptr);
	// printf("read superblock %d\n", read_success);
	if (read_success != 0){
		return -1;
	}

	uint8_t* superblock_ptr = (uint8_t*)buf_ptr;

	if (load_superblock(superblock_ptr) != 0){
		return -1;
	}
	
	if (load_fat_blocks(superblock_ptr) != 0){
		return -1;
	}

	if (load_root_directory(superblock_ptr) != 0){
		return -1;
	}

	return 0;
}

int fs_umount(void)
{
	/* TODO: Phase 1 */
	for (unsigned i = 0; i < FS_FILE_MAX_COUNT; i++){
		free(root->files[i]);
	}

	free(root);

	for (unsigned i = 0; i < sb->num_fat_blocks; i++){
		free(fat_blocks[i]);
	}

	free(fat_blocks);

	free(sb);

	block_disk_close();
	return 0;
}

int fs_info(void)
{
	/* TODO: Phase 1 */
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
			printf("%d ", fat_blocks[i]->files[j]);
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

	if (filename == NULL || *filename == 0 || strlen(filename)+1 > FS_FILENAME_LEN){
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

	// printf("%s\n", filename);
	return 0;
}

int fs_delete(const char *filename)
{
	/* TODO: Phase 2 */
	if (filename == NULL || *filename == 0 || strlen(filename)+1 > FS_FILENAME_LEN){
		fprintf(stderr, "Error in fs_delete(): invalid file name\n");
		return -1;
	}

	int matching_file_index = find_matching_filename(filename);
	if (matching_file_index == -1){
		fprintf(stderr, "Error in fs_create(): file named %s does not exist\n", filename);
		return -1;
	}

	struct File* old_file = root->files[matching_file_index];
	// TODO: remove from FAT
	old_file->filename[0] = '\0';
	old_file->file_size = 0;
	old_file->first_index = FAT_EOC;

	return 0;
}

int fs_ls(void)
{
	/* TODO: Phase 2 */
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
	printf("%s\n", filename);
	return 0;
}

int fs_close(int fd)
{
	/* TODO: Phase 3 */
	printf("%d\n", fd);
	return 0;
}

int fs_stat(int fd)
{
	/* TODO: Phase 3 */
	printf("%d\n", fd);
	return 0;
}

int fs_lseek(int fd, size_t offset)
{
	/* TODO: Phase 3 */
	printf("%d %ld\n", fd, offset);
	return 0;
}

int fs_write(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
	printf("%d %p %ld\n", fd, buf, count);
	return 0;
}

int fs_read(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
	printf("%d %p %ld\n", fd, buf, count);
	return 0;
}

