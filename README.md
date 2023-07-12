# Simple MS-FAT File System

## Overview
This program is a simplified implementation of the Microsoft File Allocation
Table (MS-FAT) file system, a file organization format used in Windows
operating systems in the 1980s and 1990s. It consists of a series of 4096-byte
data blocks stored on a virtual disk. It contains a super block, a number of
FAT blocks, a block for the root directory, and a number of blocks for the
actual files.

### Super Block
The super block is the first block in the file system, and contains its
metadata. These include the file system's signature (i.e. name), the
number of blocks it contains, the index of the first data block, etc.

### File Allocation Table
The file allocation table (FAT) is a table containing the indices for data
blocks for every file. If a file is more than 4096 bytes, its data will be
spread across multiple blocks, which may not be contiguous. Each entry in the
FAT stores the index of the first data block for each file, and the location in
the FAT of that file's next data block. In essence, the FAT is a linked list of
data block indices for each file. Like everything else in the file system, the
FAT is stored in blocks.

### Root Directory
In this implementation, there is only one directory, the root, containing a
number of files, and no subdirectories. Each file is a struct containing its
name, file size in bytes, and first block index. Internally, the root is simply
an array of file structs.

### Loading the File System
First, the file system must be mounted, which consists of loading the virtual
disk, filling out the super block, FAT, and root. To fill out the super block,
the block data from the disk, stored as a byte array, is loaded and parsed.
Then, each FAT block is read from the disk and stored in the file system's FAT.
Finally, the root directory, which is always only one block, is read in and
parsed.

### Creating and Deleting Files
To create a new file, the file system finds an empty entry in the root
directory and initializes the new file struct. To delete a file, the program
iterates through the root until it finds the file, frees all blocks associated
with it, clear the FAT of all its data blocks, and remove it from the root.

### Opening and Closing Files
To open a file, the program creates a File Descriptor, a struct containing the
file and an offset. That descriptor is then added to an array of open files.
To close a file, it is simply removed from the open files array. The same file
can be opened multiple times. Only 32 files can be open at a time.

### Reading from and Writing to Files
Reading and writing is a complicated procedure, since the virtual disk is
block-addressable, meaning that anything smaller than a block cannot be
directly accessed. To read from a file, first the program calculates how many
blocks on the disk will be read from.

The first step in the actual reading process is to check whether the read
starts at the beginning of a block or partway through. If it is the latter,
that entire block is read into a bounce buffer, then the desired part of that
buffer is added to the return array. Next, every whole block in the read, if
applicable, is read into the return array. Finally, the program checks if the
read ends partway through a block. If so, it reads into a bounce buffer like
the start. The return array, as well as how many bytes were read, is returned.

Writing is similar, but if the write requires the file to have more blocks
allocated than it currently does, new blocks need to be allocated. Since the
program already calculates how many blocks will be accessed, it can use that
and the file size to find how many new blocks need to be added. That many new
blocks are allocated and stored in the FAT, and from there the write proceeds
like the read.

### Saving Changes
Once the program is finished using the disk, they must unmount it. Changed data
is saved to the file system in much the same way as it is loaded initially.
First, each FAT block is written to the disk. Then, the root block is derived
from the internally-stored array, then written to the disk as well.
