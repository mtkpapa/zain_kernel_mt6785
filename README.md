# Information

This repository contains a slightly updated kernel tree for the **rosemary** Xiaomi device that was originally made by Zain Arbani. Also, it already contains KernelSU and SUSFS hooks.

# How to build

## Setting up the enviroment

Firstly, you need to install those packages
```
sudo apt install build-essential git curl wget bison flex zip bc cpio libssl-dev ccache python3 python-is-python3
```
Next you need to prepare a toolchain. I recommend using [this](https://github.com/ZyCromerZ/Clang/releases/download/21.0.0git-20250418-release) specific version of ZyC Clang
	
Execute this command somewhere where you want your toolchain to be
```
mkdir zyclang22 && wget -O- https://github.com/ZyCromerZ/Clang/releases/download/21.0.0git-20250418-release/Clang-21.0.0git-20250418.tar.gz | tar -xzf - -C zyclang21
```
Now you need to define it in the $PATH. Add this to your .bashrc
```
export PATH=/root/zyclang21/bin:$PATH
export LD_LIBRARY_PATH=/root/zyclang21/lib
```
Reopen Terminal or do 'source .bashrc' and you're ready to rock!

## Building kernel

Build without KernelSU
```
bash build.sh
```
	
And if you want to build it with KernelSU
```
bash build.sh ksu
```
	
The output .zip will be in the root source's directory