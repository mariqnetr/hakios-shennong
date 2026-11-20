# userland/ in the shennong-ios volume is not in git, so these are scripted edits (as fix-userland.py); each once
U = '/ios/hackios/userland/'
p = U + 'build.sh'
s = open(p).read()
if 'bluetool' not in s:
    a = '[ "${WIFID-1}" != 0 ] && IOS_JOBS="$IOS_JOBS com.apple.wifid"\n'
    assert a in s
    s = s.replace(a, a + '# Bluetooth (shennong: bsd/dev/shennong_bt.c, bt=4): iOS\'s bluetoothd on the kernel\'s /dev/cu.bluetooth. BlueTool, which would\n# load a Broadcom chip, is src/bluetool.c. It needs the registry\'s "bluetooth", which only that kernel makes: bluetoothd exits at once without.\nexport ALLOW="${ALLOW:-} bluetooth BTServer"\nIOS_JOBS="$IOS_JOBS com.apple.bluetoothd com.apple.bluetoothuserd"\n', 1)
    a = '	cp com.redwood.demo.plist com.redwood.wifi.plist com.redwood.thermald.plist root/LaunchDaemons/\n'
    assert a in s
    a2 = '[ -n "$IOS_JOBS" ] && OSLOG=${OSLOG-com.apple.backboardd} ./mkjobs.py ${REST:+--rest} $VOLUME $IOS/rootfs root/LaunchDaemons $IOS_JOBS\n'
    assert a2 in s
    s = s.replace(a2, a2 + '	cp com.apple.BlueTool.plist root/LaunchDaemons/com.apple.BlueTool.plist\n', 1)
    a3 = '		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m\n'
    assert a3 in s
    s = s.replace(a3, a3 + '		$DYN -o obj/bluetool src/bluetool.c\n', 1)
    a4 = '	[ -f obj/net_test ] && cp obj/net_test root/bin/\n'
    assert a4 in s
    s = s.replace(a4, a4 + '	[ -f obj/bluetool ] && cp obj/bluetool root/bin/\n', 1)
    open(p, 'w').write(s)
    print('build.sh: bluetoothd and BlueTool in')

s = open(p).read()
if 'BT_HCI_TRANSPORT' not in s:
    a = '	cp com.apple.BlueTool.plist root/LaunchDaemons/com.apple.BlueTool.plist\n'
    assert a in s
    s = s.replace(a, a + """	python3 -c "
import plistlib
p='root/LaunchDaemons/com.apple.bluetoothd.plist'
d=plistlib.load(open(p,'rb'))
d.setdefault('EnvironmentVariables',{})['BT_HCI_TRANSPORT']='H4:POSIX:com.redwood.uart.bluetooth,3000000'
plistlib.dump(d,open(p,'wb'))"
""", 1)
    open(p, 'w').write(s)
    print('build.sh: bluetoothd told its transport')

# The keyboard (src/inject_bt.c in bluetoothd -> the input device -> src/inject.c in backboardd), the audio plug-in
s = open(p).read()
if 'inject_bt' not in s:
    a = '/redwood/lib/libredwood-media.dylib mediaserverd\n'
    assert a in s
    s = s.replace(a, a + '/redwood/lib/libredwood-bt.dylib bluetoothd\n', 1)
    a = '		$DYN -dynamiclib -install_name /redwood/lib/libredwood-sb.dylib'
    assert a in s
    s = s.replace(a, '		$DYN -dynamiclib -install_name /redwood/lib/libredwood-bt.dylib -framework CoreFoundation -framework IOKit -o obj/libredwood-bt.dylib src/inject_bt.c\n' + a, 1)
    a = '	[ -f obj/libredwood-media.dylib ] && cp obj/libredwood-media.dylib root/lib/'
    assert a in s
    s = s.replace(a, '	[ -f obj/libredwood-bt.dylib ] && cp obj/libredwood-bt.dylib root/lib/\n' + a, 1)
    a = '[ -d root/lib/RedwoodAudio.driver ] && ln -s /redwood/lib/RedwoodAudio.driver root/hal/RedwoodAudio.driver\n'
    assert a in s
    s = s.replace(a, a + '''# Apple's Bluetooth audio plug-in (BTAudioHALPlugin, fetched from the iOS volume into ../ios165/bt-audio)
if [ -d ../ios165/bt-audio/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver ]; then
	cp -R ../ios165/bt-audio/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver root/lib/
	ln -s /redwood/lib/BTAudioHALPlugin.driver root/hal/BTAudioHALPlugin.driver
fi
''', 1)
    open(p, 'w').write(s)
    print('build.sh: libredwood-bt and the Bluetooth audio plug-in in')
h = U + 'src/redwood_input.h'
s = open(h).read()
if 'REDWOOD_KEY_DOWN' not in s:
    a = '#define REDWOOD_BUTTON_UP       5\n'
    assert a in s
    s = s.replace(a, a + '#define REDWOOD_KEY_DOWN         6       /* a key of a keyboard: x its usage, y its usage page */\n#define REDWOOD_KEY_UP           7\n', 1)
    open(h, 'w').write(s)
i = U + 'src/inject.c'
s = open(i).read()
if 'REDWOOD_KEY_DOWN' not in s:
    a = '		if (t.type > REDWOOD_TOUCH_UP) {\n			if (buttons != NULL) {'
    assert a in s
    s = s.replace(a, '''		if (t.type == REDWOOD_KEY_DOWN || t.type == REDWOOD_KEY_UP) {
			/* a Bluetooth keyboard (src/inject_bt.c): its service only once one has been heard */
			static IOHIDServiceClientRef keyboard;

			if (keyboard == NULL) {
				keyboard = make_service(client, "Bluetooth keyboard", 1, 6, false);
				say("redwood: input: keyboard service %p\\n", (void *)keyboard);
			}
			if (keyboard != NULL) {
				IOHIDEventRef event = IOHIDEventCreateKeyboardEvent(NULL, t.time, t.y, t.x, t.type == REDWOOD_KEY_DOWN, 0);

				IOHIDVirtualServiceClientDispatchEvent(keyboard, event);
				CFRelease(event);
			}
		} else if (t.type > REDWOOD_TOUCH_UP) {
			if (buttons != NULL) {''', 1)
    open(i, 'w').write(s)
n = U + 'src/init.c'
s = open(n).read()
if 'Bluetooth audio' not in s:
    a = '	run_rc(in_image("/etc/rc"));\n'
    assert a in s
    s = s.replace(a, '''	/* the audio plug-ins of this image, Bluetooth audio's among them, on a RAM root too (flash_root() does its own) */
	if (!flash_root_mode) {
		bind_dir(in_image("/hal"), "/System/Library/Audio/Plug-Ins/HAL");
	}
''' + a, 1)
    open(n, 'w').write(s)
