HERE=${PWD}

VERSION=branches/release_38
#VERSION=trunk

LLVM="http://llvm.org/svn/llvm-project/llvm/${VERSION}"
CLANG="http://llvm.org/svn/llvm-project/cfe/${VERSION}"
CTOOLS="http://llvm.org/svn/llvm-project/clang-tools-extra/${VERSION}"
COMPILER-RT = "http://llvm.org/svn/llvm-project/compiler-rt/${VERSION}"

all:  build

llvm:
	svn co ${LLVM} llvm

../gold/binutils/include:
	make -C ../gold 

llvm/tools/clang: llvm
	cd llvm/tools/; svn co ${CLANG} clang

llvm/tools/clang/tools: llvm/tools/clang
	cd llvm/tools/clang/tools; svn co ${CTOOLS} clang

llvm/projects/compiler-rt: llvm
	cd llvm/projects/; svn co ${COMPILER-RT} compiler-rt

theworks: llvm/projects/compiler-rt llvm/tools/clang/tools llvm/tools/clang


build: theworks
	mkdir build; cd build; CC=wllvm CXX=wllvm++ cmake -DLLVM_BINUTILS_INCDIR=${HERE}/../gold/binutils/include ../llvm; make -j4



check:   build
	cd build; make check-libcxx check-libcxxabi

clean:
	rm -rf build

spotless: clean
	rm -rf llvm


.PHONY: clean spotless check theworks


