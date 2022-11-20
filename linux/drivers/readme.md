# How to add VEYE mipi cameras on RK356x
### Introduction
Use firefly ROC-RK3566-PC as an example.
This document references this [link](https://wiki.t-firefly.com/en/ROC-RK3566-PC/ "link") from this Firefly.Please check it together with this documentation.
### Build the standard version
Refer to this [link](https://wiki.t-firefly.com/en/ROC-RK3566-PC/prepare_compile_linux.html "link"), Build the build environment.
Refer to this [link](https://wiki.t-firefly.com/en/ROC-RK3566-PC/ubuntu_compile.html "link") to compile the standard version.
### Patch the code
1. Put the camera driver files into the kernel/drivers/media/i2c directory, and modify the Kconfig and Makefile files to add driver options.
2. Put the dts file into the kernel\arch\arm64\boot\dts\rockchip directory.
3. Modify the . /arch/arm64/configs/firefly_linux_defconfig file to add VEYE camera option.

### Compile firmware to support VEYE camera version
To compile the kernel and dtb in the same way as the standard version, execute the following command.
`./build.sh extboot`
Execute the following command to compile and get the installation package.
`./build.sh kerneldeb`

### Install
Copy the installation package to the motherboard and execute.
```
sudo dpkg -i linux-image-4.19.232_4.19.232-20_arm64.deb
```


