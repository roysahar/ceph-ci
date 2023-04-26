#!/bin/sh -ex

# Run qemu-iotests against rbd. These are block-level tests that go
# through qemu but do not involve running a full vm. Note that these
# require the admin ceph user, as there's no way to pass the ceph user
# to qemu-iotests currently.

testlist='001 002 003 004 005 008 009 010 011 021 025 032 033'

git clone https://github.com/qemu/qemu.git
cd qemu


if grep -iqE '(bionic|focal|jammy)' /etc/os-release; then
    # Bionic requires a matching test harness
    git checkout v2.11.0
elif grep -iqE '(xenial|platform:el8)' /etc/os-release; then
    # Xenial requires a recent test harness
    git checkout v2.3.0
else
    # use v2.2.0-rc3 (last released version that handles all the tests
    git checkout 2528043f1f299e0e88cb026f1ca7c40bbb4e1f80
fi

cd tests/qemu-iotests
mkdir bin
# qemu-iotests expects a binary called just 'qemu' to be available
if [ -x '/usr/bin/qemu-system-x86_64' ]
then
    QEMU='/usr/bin/qemu-system-x86_64'

    # Bionic (v2.11.0) tests expect all tools in current directory
    ln -s $QEMU qemu
    ln -s /usr/bin/qemu-img
    ln -s /usr/bin/qemu-io
    ln -s /usr/bin/qemu-nbd
else
    QEMU='/usr/libexec/qemu-kvm'
fi
ln -s $QEMU bin/qemu

# this is normally generated by configure, but has nothing but a python
# binary definition, which we don't care about.  for some reason it is
# not present on trusty.
touch common.env

# TEST_DIR is the pool for rbd
TEST_DIR=rbd PATH="$PATH:$PWD/bin" ./check -rbd $testlist

cd ../../..
rm -rf qemu
