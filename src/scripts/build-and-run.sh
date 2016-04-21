#!/bin/bash - 
#=============================================================================
# Build NFS-Ganesha as TC Server and run it
# 
# by Ming Chen, v.mingchen@gmail.com
#=============================================================================

set -o nounset                          # treat unset variables as an error
set -o errexit                          # stop script if command fail
IFS=$' \t\n'                            # reset IFS
unset -f unalias                        # make sure unalias is not a function
\unalias -a                             # unset all aliases
ulimit -H -c 0 --                       # disable core dump
hash -r                                 # clear the command path hash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

SAVEDIR=$PWD

cd $DIR/../..
git submodule update --init
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ../src 
make
make install

mkdir -p /etc/ganesha
cp $DIR/../config_samples/tcserver.ganesha.conf /etc/ganesha

# create the NFS directory that is going to be exported
mkdir -p /tcserver
echo "hello TC" > /tcserver/hello.txt

cp $DIR/run-tcserver.sh .
./run-tcserver.sh

sleep 60 # for grace period

cd $SAVEDIR
