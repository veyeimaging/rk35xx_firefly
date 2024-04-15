# How to add VEYE mipi cameras on RK358x
### Introduction
Use firefly ROC-RK3566-PC as an example. RK358x is now using linux kernel version 5.10.
This document references this [link](https://wiki.t-firefly.com/en/ROC-RK3566-PC/ "link") from this Firefly.Please check it together with this documentation.
### Build the standard version
Refer to this [link](https://wiki.t-firefly.com/en/ROC-RK3566-PC/linux_compile_linux5.10.html "link"), Build the build environment.
Refer to this [link](https://wiki.t-firefly.com/en/ROC-RK3566-PC/linux_compile_linux5.10.html "link") to compile the standard version.
### Patch the code
1. Put the camera driver files into the kernel/drivers/media/i2c directory, and modify the Kconfig and Makefile files to add driver options.
2. Put the dts file into the kernel\arch\arm64\boot\dts\rockchip directory.
  Note that for different SDK versions, the first few lines of different DTSI files need to be modified to select the camera driver to be enabled - either the VEYE series or the MV series.
3. For RK3566, Modify the . /arch/arm64/configs/firefly-linux.config file to add VEYE camera option.
### Compile firmware to support VEYE camera version

`./build.sh`



