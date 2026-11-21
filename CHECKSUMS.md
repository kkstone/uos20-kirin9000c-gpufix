# 最终 deb 校验和（MD5）

| 文件 | 大小 | MD5 |
|---|---|---|
| releases/eglrun_2.0.2_arm64.deb | 32K | e916b322dc1054bbc8f7438be3bc1d54 |
| releases/eglrun_1.0.15_arm64.deb | 52K | d1457a61de0f868d1a5267b9b25d0a72 |
| releases/uoshim-wayland-im_1.2_arm64.deb | 22K | 59fc2730b6b92add56d653f630d8a2b8 |
| releases/google-chrome-eglrun_157.0.8103.78-2_arm64.deb | 153M (LFS) | e52663de18b0aab939da46572549af03 |
| releases/firefox.appimage_160.0-2_arm64.deb | 91M (LFS) | d2b6b3da7bd4098782a50ccbf19cf97f |
| eglrun/opt/eglrun/lib/icd-wrap.so | - | 5a0b81e4a7412d8283f5876c1a24b084 |
| eglrun/opt/eglrun/lib/vaapi-shim.so | - | a2f85c6e37effe6aea27cb6b2b3ed953 |

## Firefox 160.0-2 关键文件校验和（MD5）

| 文件 | MD5 | 说明 |
|---|---|---|
| firefox/opt/mozilla-firefox/lib/wheel_shim.so | 1f91a3af974f11ae56d39927853301e8 | 自包含实现，零 `wl_*` 外部符号 |
| firefox/usr/local/bin/firefox | 604f2417264445deb93ed0c4a8543c35 | 每次启动 DPI→光标尺寸同步 |
| firefox/opt/mozilla-firefox/src/firefox-launcher.sh | 604f2417264445deb93ed0c4a8543c35 | 与部署启动器一致 |
| firefox/opt/mozilla-firefox/src/wheel-shim/wheel_shim.c | dbca1479114b821531802879bf941bcb | wheel_shim 源码 |
| firefox/opt/mozilla-firefox/src/wheel-shim/wayland_mini.h | 0bdd18018130d8ef44930e6fa4640c2d | 自包含 ABI 头 |
