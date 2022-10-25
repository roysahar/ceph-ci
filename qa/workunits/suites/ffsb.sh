#!/usr/bin/env bash

set -e

mydir=`dirname $0`

testdir='./'
if [ $# -ge 1 ]
then
    testdir=$1
fi
pushd $testdir

# try it again if the clone is slow and the second time
trap -- 'retry' EXIT
retry() {
    rm -rf ffsb
    # double the timeout value
    timeout 3600 git clone git://git.ceph.com/ffsb.git --depth 1
}
rm -rf ffsb
timeout 1800 git clone git://git.ceph.com/ffsb.git --depth 1
trap - EXIT

cd ffsb
./configure
make
cd ..
mkdir tmp
cd tmp

for f in $mydir/*.ffsb
do
    ../ffsb/ffsb $f
done
cd ..
rm -r tmp ffsb*
popd

