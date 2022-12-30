# How to add VEYE mipi cameras on RK358x(Android)
### Introduction
Use firefly ROC-RK3588S-PC as an example.
This document references this [link](https://wiki.t-firefly.com/en/ROC-RK3588S-PC/ "link") from this Firefly.Please check it together with this documentation.
### Build the standard version
Refer to this [link](https://wiki.t-firefly.com/en/ROC-RK3588S-PC/android_prepare_compile_android.html "link"), Build the build environment.
Refer to this [link](https://wiki.t-firefly.com/en/ROC-RK3588S-PC/android_compile_android12.0_firmware.html "link") to compile the standard version.
### Patch the code
1. Put the camera driver files into the kernel/drivers/media/i2c directory, and modify the Kconfig and Makefile files to add driver options.
2. Put the dts file into the kernel\arch\arm64\boot\dts\rockchip directory.
3. Modify the . /arch/arm64/configs/firefly_defconfig file to add VEYE camera option.
4. Put camera3_profiles_rk3588.xml file into hardware\rockchip\camera\etc\camera directory.

### Compile firmware to support VEYE camera version
To compile the kernel and dtb in the same way as the standard version, execute the following command.
```
./FFTools/make.sh -d roc-rk3588s-pc -j8 -l roc_rk3588s_pc-userdebug
```
Packaged into unified firmware update.img
After compilation, you can use Firefly official scripts to package into unified firmware, execute the following command：

`./FFTools/mkupdate/mkupdate.sh -l roc_rk3588s_pc-userdebug`

### Burn new android image in the same way as the standard version



