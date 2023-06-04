#!/bin/bash

make
rm -f disk.fs
rm -f file_fs
./fs_make.x disk.fs $2
./test_fs.x script disk.fs scripts/$1.script