#!/bin/bash

SRC=src/*.c
BUILD_DIR=build

[ -d $BUILD_DIR ] || mkdir BUILD_DIR

for fx in $SRC ; do
	bfx=$(basename $fx)
	gcc -g -o build/${bfx/.c/} -Isrc $fx -lrt src/common/*.c
done

