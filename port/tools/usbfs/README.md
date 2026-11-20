# usbfs: the phone's files on the Mac, read and write, over the USB cable

While a kernel runs, the loader keeps the USB (fastboot) and now also offers a byte pipe to the
kernel: `oem pipe-w:HEX` / `oem pipe-r:HEX` / `oem pipe-s` (xnuloader/loader.c, struct loader_pipe,
`pipe_pa=` on the kernel's command line). The kernel's end is `/dev/hostpipe` (bsd/dev/redwood_ufs.c).

* `usbfsd.c` runs on iOS as root (build/boot-bin, started from rc.jobs at boot) and does file
  requests on the whole filesystem with plain POSIX calls.
* `bridge/` (Go, libusb) is the Mac's end: it talks to usbfsd through the loader, serves the files as
  NFSv3 on 127.0.0.1 and mounts them on ~/iPhoneX (Finder shows it). Ctrl-C unmounts.

Build:

    xcrun -sdk iphoneos clang -target arm64-apple-ios16.0 -O2 -o usbfsd usbfsd.c && codesign -f -s - usbfsd
    brew install libusb pkgconf && (cd bridge && go build -o ../usbfs .)

Use: `./xl run` (with an XL_SECS long enough), then `usbfs` in another terminal. The bridge takes
xl's lock (/tmp/hackios-phone.lock) only while it talks, so `xl stop`, `xl screenshot` keep working.
`usbfs -local ./usbfsd-mac` (usbfsd built for the Mac) tries all of it without the phone.
