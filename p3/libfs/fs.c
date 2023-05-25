#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define SUPERBLOCK_SIG_LEN 8
#define SUPERBLOCK_PREPAD_LEN SUPERBLOCK_SIG_LEN+2+2+2+2+1
#define SUPERBLOCK_PAD_LEN BLOCK_SIZE-SUPERBLOCK_PREPAD_LEN

#define FATBLOCK_FILENO 2048

struct SuperBlock{
	int8_t signature[8];
	uint16_t num_blocks;
	uint16_t root_index;
	uint16_t data_start_index;
	uint16_t num_data_blocks;
	uint8_t num_fat_blocks;
	uint8_t padding[SUPERBLOCK_PAD_LEN];
};

struct Root{
	int8_t filename[FS_FILENAME_LEN];
	uint32_t file_size;
	uint16_t first_index;
	int8_t padding[10];
};

struct FATBlock{
	int16_t files[FATBLOCK_FILENO];
};

/* TODO: Phase 1 */

struct SuperBlock sb;
struct FATBlock** fat_blocks;

uint16_t concatenate_two_bytes(int8_t byte1, int8_t byte2){
	int16_t result = 0;
	result += 256*byte2;
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

int load_superblock(int8_t* superblock_ptr){
	if (superblock_ptr == NULL){
		fprintf(stderr, "Error in fs_mount(): pointer to superblock is null\n");
		return -1;
	}

	// for (unsigned i = 0; i < 20; i++){
	// 	// if (i <= SUPERBLOCK_SIG_LEN){
	// 	// 	printf("%c", *(superblock_ptr+i));
	// 	// } else 
	// 	// 	printf("%d", *(superblock_ptr+i));
	// 	printf("%d ", *(superblock_ptr+i));
	// }
	// printf("\n");

	// load each byte of the signature in one at a time
	for (unsigned i = 0; i < SUPERBLOCK_SIG_LEN; i++){
		sb.signature[i] = *(superblock_ptr++);
	}

	// this should concatenate the next two bytes of the buffer
	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb.num_blocks = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb.num_blocks != block_disk_count()){
		fprintf(stderr, "Error in fs_mount(): num_blocks read from superblock does not match number on disk\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb.root_index = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb.root_index == 0){
		fprintf(stderr, "Error in fs_mount(): root index is where superblock should be\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb.data_start_index = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb.data_start_index == 0){
		fprintf(stderr, "Error in fs_mount(): data start index is where superblock should be\n");
		return -1;
	}

	if (sb.data_start_index == sb.root_index){
		fprintf(stderr, "Error in fs_mount(): root and data start have the same index\n");
		return -1;
	}

	// printf("%d %d\n", *superblock_ptr, *(superblock_ptr+1));
	sb.num_data_blocks = concatenate_two_bytes(*superblock_ptr, *(superblock_ptr+1));
	superblock_ptr += 2;

	if (sb.num_data_blocks >= sb.num_blocks){
		fprintf(stderr, "Error in fs_mount(): more data blocks than total blocks\n");
		return -1;
	}

	// printf("%d\n", *superblock_ptr);
	sb.num_fat_blocks = *(superblock_ptr);
	superblock_ptr++;

	if (sb.num_fat_blocks >= sb.num_blocks){
		fprintf(stderr, "Error in fs_mount(): more FAT blocks than total blocks\n");
		return -1;
	}

	for (unsigned i = SUPERBLOCK_PREPAD_LEN; i < BLOCK_SIZE; i++){
		sb.padding[i] = *(superblock_ptr++);

		if (sb.padding[i] != 0){
			fprintf(stderr, "Error in fs_mount(): incorrect padding formatting\n");
			return -1;
		}
	}

	return 0;
}

int load_fat_blocks(int8_t* fat_block_ptr){
	fat_blocks = malloc(sizeof(fat_blocks));
	if (fat_blocks == NULL){
		return -1;
	}

	for (unsigned i = 0; i < sb.num_fat_blocks; i++){
		struct FATBlock* fat_block = malloc(sizeof(fat_block));
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
	int8_t superblock_buffer[BLOCK_SIZE];
	void* buf_ptr = &superblock_buffer;

	int read_success = block_read(0, buf_ptr);
	// printf("read superblock %d\n", read_success);
	if (read_success != 0){
		return -1;
	}

	int8_t* superblock_ptr = (int8_t*)buf_ptr;

	if (load_superblock(superblock_ptr) != 0){
		return -1;
	}
	
	// if (load_fat_blocks(superblock_ptr) != 0){
	// 	return -1;
	// }

	return 0;
}

int fs_umount(void)
{
	/* TODO: Phase 1 */
	// for (unsigned i = 0; i < sb.num_fat_blocks; i++){
	// 	free(fat_blocks[i]);
	// }

	// free(fat_blocks);

	block_disk_close();
	return 0;
}

int fs_info(void)
{
	/* TODO: Phase 1 */
	printf("Signature: ");
	for (unsigned i = 0; i < 8; i++){
		printf("%c", sb.signature[i]);
	}

	printf("\n");

	printf("Number of blocks: %d\n", sb.num_blocks);
	printf("Root index: %d\n", sb.root_index);
	printf("Data start index: %d\n", sb.data_start_index);
	printf("Number of data blocks: %d\n", sb.num_data_blocks);
	printf("Number of FAT blocks: %d\n", sb.num_fat_blocks);
	printf("Padding: ");
	for (unsigned i = 0; i < SUPERBLOCK_PAD_LEN; i++){
		printf("%d", sb.padding[i]);
	}

	printf("\n");

	printf("Number of blocks according to block_disk_count: %d\n", block_disk_count());
	return 0;
}

int fs_create(const char *filename)
{
	/* TODO: Phase 2 */
<<<<<<< HEAD
	printf("%s\n", filename);
	return 0;
=======
	int MAX_ENTRIES[] = 128;
	struct Root root_dir;

	// When directory is empty
	root_dir.file_size = 0;
	root_dir.first_index = "FAT_EOC";

	// Find free entry in the directory
	for (unsigned i = 0; i < MAX_ENTRIES; i++){
		if (root_dir.first_index == "FAT_EOC"){
			root_dir.filename[16] = filename;
			//SET FILE SIZE
		}
		else if (root_dir.first_index < MAX_ENTRIES){
			//find empty entry to add file
		}
		
	}
>>>>>>> refs/remotes/origin/main
}

int fs_delete(const char *filename)
{
	/* TODO: Phase 2 */
	printf("%s\n", filename);
	return 0;
}

int fs_ls(void)
{
	/* TODO: Phase 2 */
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

