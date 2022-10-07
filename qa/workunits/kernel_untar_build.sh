#!/usr/bin/env bash

set -e

# for kernel debugging
mount -t debugfs none /sys/kernel/debug
echo "file fs/ceph/* +pf" > /sys/kernel/debug/dynamic_debug/control

wget -O linux.tar.gz http://download.ceph.com/qa/linux-5.4.tar.gz

mkdir t
cd t
tar xzf ../linux.tar.gz
cd linux*
make defconfig
make -j`grep -c processor /proc/cpuinfo` V=1
cd ..
if ! rm -rv linux* ; then
    echo "uh oh rm -r failed, it left behind:"
    find .
    exit 1
fi
cd ..
rm -rv t linux*
