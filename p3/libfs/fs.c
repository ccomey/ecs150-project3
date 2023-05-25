#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

struct SuperBlock{
	int8_t signature[8];
	uint16_t num_blocks;
	int16_t root_index;
	uint16_t data_start_index;
	uint16_t num_data_blocks;
	uint8_t num_fat_blocks;
	uint8_t padding[4079];
};

struct Root{
	int8_t filename[16];
	uint32_t file_size;
	uint16_t first_index;
	int8_t padding[10];
};

struct FATBlock{
	int16_t files[2048];
};

/* TODO: Phase 1 */

int fs_mount(const char *diskname)
{
	/* TODO: Phase 1 */

	// open the disk
	block_disk_open(diskname);

	// load the buffer containing the superblock data
	void* superblock_buffer;
	block_read(0, superblock_buffer);
	int8_t* superblock_ptr = (int8_t*)superblock_buffer;

	// load each byte of the signature in one at a time
	struct SuperBlock sb;
	for (unsigned i = 0; i < 8; i++){
		sb.signature[i] = *(superblock_ptr++);
	}

	// this should concatenate the next two bytes of the buffer
	sb.num_blocks = *superblock_ptr << 8 | *(++superblock_ptr);
	superblock_ptr++;

	sb.root_index = *superblock_ptr << 8 | *(++superblock_ptr);
	superblock_ptr++;

	sb.data_start_index = *superblock_ptr << 8 | *(++superblock_ptr);
	superblock_ptr++;

	sb.num_data_blocks = *superblock_ptr << 8 | *(++superblock_ptr);

	sb.num_fat_blocks = *(++superblock_ptr);

	// this should be all of the padding
	for (unsigned i = 17; i < 4079; i++){
		sb.signature[i] = *(superblock_ptr++);
	}


	// this should be moved to fs_unmount once this function is finished
	block_disk_close();
}

int fs_umount(void)
{
	/* TODO: Phase 1 */
}

int fs_info(void)
{
	/* TODO: Phase 1 */
}

int fs_create(const char *filename)
{
	/* TODO: Phase 2 */
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
}

int fs_delete(const char *filename)
{
	/* TODO: Phase 2 */
}

int fs_ls(void)
{
	/* TODO: Phase 2 */
}

int fs_open(const char *filename)
{
	/* TODO: Phase 3 */
}

int fs_close(int fd)
{
	/* TODO: Phase 3 */
}

int fs_stat(int fd)
{
	/* TODO: Phase 3 */
}

int fs_lseek(int fd, size_t offset)
{
	/* TODO: Phase 3 */
}

int fs_write(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
}

int fs_read(int fd, void *buf, size_t count)
{
	/* TODO: Phase 4 */
}

