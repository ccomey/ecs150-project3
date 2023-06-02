#!/bin/bash

make
rm -f disk.fs
rm -f file_fs
./fs_make.x disk.fs 10
./test_fs.x script disk.fs scripts/s1.script