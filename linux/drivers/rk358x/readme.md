# How to add VEYE mipi cameras on RK358x
### Introduction
Use firefly ROC-RK3588S-PC as an example. RK358x is now using linux kernel version 5.10.
This document references this [link](https://wiki.t-firefly.com/en/ROC-RK3588S-PC/ "link") from this Firefly.Please check it together with this documentation.
### Build the standard version
Refer to this [link](https://wiki.t-firefly.com/en/ROC-RK3588S-PC/linux_compile_ubuntu.html "link"), Build the build environment.
Refer to this [link](https://wiki.t-firefly.com/en/ROC-RK3588S-PC/linux_compile_ubuntu.html "link") to compile the standard version.
### Patch the code
1. Put the camera driver files into the kernel/drivers/media/i2c directory, and modify the Kconfig and Makefile files to add driver options.
2. Put the dts file into the kernel\arch\arm64\boot\dts\rockchip directory.
3. For RK3588, Modify the . /arch/arm64/configs/firefly-linux.config file to add VEYE camera option.
### Compile firmware to support VEYE camera version
To compile the kernel and dtb in the same way as the standard version, execute the following command.
`./build.sh extboot`
Execute the following command to compile and get the installation package.
`./build.sh kerneldeb`



