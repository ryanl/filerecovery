#!/bin/sh

if [ $(uname -m | grep '64') ];then
    echo "Building..."
else
    echo "Warning: This code requires a 64-bit processor and OS in order to"
    echo "         work with files more than, say, 2 GB."
    echo "Building..."

    # If you are stuck with a 32-bit machine, maybe this is something you could
    # fix :)
fi

g++ rescue.cpp -O2 -o rescue
