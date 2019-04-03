
# Build Rockit in Linux 3308

## 1 Config Rockit for Linux 3308

```
$ cd rk3308_linux
$ cp rockit_path.git ./external/rockit_dev -rf    // copy rockit sources to [external/rockit_dev]
$ cp ./external/rockit_dev/build/buildroot ./buildroot/package/rockit -rf  // copy rockit config to package
$ cd buildroot/output/rockchip_rk3308_release
$ make rockit-rebuild
```

## 2 Update Rockit

```
$ rm build/rockit/.stamp_built
$ make rockit
adb push rt_player_test /userdata/bin/
adb shell
$ export LD_LIBRARY_PATH=/userdata/lib64   // old ffmpeg is bad, link new ffmpeg
$ export LD_LIBRARY_PATH=/oem/lib
$ cd /userdata/bin/
$ chmod 777 rt_player_test

Current Audio File: /userdata/bin/start.mp3
$ rm /oem/lib/librt_player_shared.so
$ mv /data/librt_player_shared.so /oem/lib
$ ./rt_player_test
```

## 3 Build 3308 Fireware

```
$ cd ~/code/git-3308-rns/MINI-S1-RK3308
$ cd device/rockchip/rk3308/soundai-cmcc-hemiao-3.04/
$ cd ~/code/git-3308-rns/MINI-S1-RK3308
$ ./build.sh 
```

## 4 Config Ethernet

```
$ adb push wpa_supplicant.conf /data/cfg/
$ ifconfig
```

## 5 Config gdb for linux

```
$ cd ${SDK_ROOT}/buildroot/output/rockchip_rk3308_soundai_release/
$ make menuconfig
$ -- build option
$ -- -- build packages with debugging symbols [y]
$ -- -- strip target binaries [n]
$ -- Toolchain
$ -- -- Build cross gdb for the host -> GDB debugger Version (gdb 7.11.x) [y]
$ -- Target packages
$ -- --  Debugging, profiling and benchmark --> gdb [y] -- full debugger [y]
$ -- --  Debugging, profiling and benchmark --> strace [y]
$ -- --  Development tools --> cppunit/gperf [n]
```