# ELAN1200 04F3:3022 Touchpad

## Warning
The code in the repository has been tested and being used with the touchpad installed in **Asus UX310UQ** laptops, **vendor/device 04F3:3022**. The OS is Debian 10 with backports enabled. I can't guarantee it working with other models, revisions, Linux distibutions etc.


### The problem
<p>
<img src="http://mishurov.co.uk/images/github/linux_elan1200_touchpad/state1.png" width="350"/><br/>
<em>Normal two finger touch state</em>
<p/>
<p>
<img src="http://mishurov.co.uk/images/github/linux_elan1200_touchpad/state2.png" width="350"/><br/>
<em>When fingers are close enough, the touchpad recognises it as one touch and sends a release report for the second contact</em>
<p/>
<img src="http://mishurov.co.uk/images/github/linux_elan1200_touchpad/state3.png" width="350"/><br/>
<em>When fingers go apart again, the touchpad <strong>sends another release for the remaining contact and immediately sends a report with two fingers</strong> touching the surface</em>
<p/>
The aforementioned redundant release report results in random right-click events during scrolling when "double tap" and "two-finger scroll" are enabled.

### The solution
The repository contains three options to resolve this issue, all of them are basically do the same:

- Got the release report from the hardware
- Save the event data and set timer for a very short time fraction
- If next event is immediatelty received, then the release was the malicious one, ignore it
- If there're no subsecuent events, and the timer is triggered, the release was correct, so emit the saved event

All the solutions have some issuses and limitations yet nonetheless they minimise the annoying behaviour of the touchpad.

#### Option one
Patch the Synaptics Xorg driver. It would be something like that:
```sh
wget https://www.x.org/pub/individual/driver/xf86-input-synaptics-1.9.1.tar.bz2
tar xf ./xf86-input-synaptics-1.9.1.tar.bz2
rm ./xf86-input-synaptics-1.9.1.tar.bz2
cd xf86-input-synaptics-1.9.1/
git apply /path/to/elan1200_1.9.1.diff
./configure
make
./libtool --mode=install /usr/bin/install -c src/synaptics_drv.la /path/to/somewhere/
mkdir -p ./rollback
cp /usr/lib/xorg/modules/input/synaptics_drv.so ./rollback/
sudo mv /path/to/somewhere/synaptics_drv.so /usr/lib/xorg/modules/input/
```
Obviously it is limited to Xorg and Synaptics driver and since I've hacked into the driver's source code it doesn't work ideally.

#### Option two
Use the userspace driver. It can work without hid-multitouch module. It takes raw HID data from a `/dev/hidraw*` device, creates a virtual `/dev/input/event*` device using the `uinput` module and reports input events according to the Linux Multi-touch (MT) Protocol Type B specification.
```sh
gcc -o hid_elan1200 hid_elan1200.c -lrt -lpthread
```
Possibly delay time should be adjusted adding `-DMEASURE_TIME` flag to gcc will print relevant time, when moving fingers close to each other and appart without lifting.

The directory also contains example Xorg configurations for Synaptics and Libinput drivers which ignore the real device and use the virtual device and a simpe systemd service file for autoload, optionally `hid-multitouch` module can be blacklisted.
```sh
sudo cp ./hid_elan1200 /usr/local/bin/
sudo cp elan1200.service /etc/systemd/system/
sudo chmod 644 /etc/systemd/system/elan1200.service
sudo systemctl enable elan1200.service
```

The `mirror_elan1200.c` in the directory just mirrors input events from the input device created by hid-multitouch without any modifications. It's my previous attempt to filter hardware reports in userspace.

#### Option three
Use the kernel module. Technically it does the same as the userspace driver, the difference is in an API. Linux Kernel's API tends to change, I use Debian stable with backports, the only kernel I can test is that one from the distribution. The latest version I tested it with is 5.8. Installation is typical as for any other module. Timings can also be measured compiling the module with the command `make CFLAGS=-DMEASURE_TIME` and watching prints in `dmesg -w`.

The directory also contains `dkms.conf` for installing and auto-recompiling during kernel updates.
```sh
# create module src dir and copy files
sudo mkdir /usr/src/hid-elan1200-1.0
sudo cp dkms.conf Makefile hid-elan1200.c /usr/src/hid-elan1200-1.0/

# add to dkms and install
sudo dkms install hid-elan1200/1.0

# remove from dkms
sudo dkms remove hid-elan1200/1.0 --all
sudo rm -r /usr/src/hid-elan1200-1.0
```

Blacklisting `hid-multitouch` may be needed. Xorg conf files are similar to the userspace driver, difference is in the name of the exposed input device.

## Misc for ASUS UX310UQ
![ScreenShot](http://mishurov.co.uk/images/github/linux_elan1200_touchpad/pm.png)
<br/><br/>
There's also an ACPI problem related to the laptop. When the battery is fully charged, it goes into the state of "discharging at zero rate". XFCE Power Manager sets the icon which doesn't reflect if power cable is connected. I made a patch `asus_ux310u_1.6.1.diff` in the corresponding directory, for xfce4-power-manager-1.6.1 which shows a correct icon when an AC cable is plugged in.
```sh
wget http://http.debian.net/debian/pool/main/x/xfce4-power-manager/xfce4-power-manager_1.6.1.orig.tar.bz2
tar xf xfce4-power-manager_1.6.1.orig.tar.bz2
cd xfce4-power-manager-1.6.1
git apply /path/to/asus_ux310u_1.6.1.diff
./autogen.sh --disable-silent-rules --with-backend=linux
./configure --disable-silent-rules --with-backend=linux
sudo make install
```

References:
- https://www.kernel.org/doc/html/latest/input/multi-touch-protocol.html
- https://www.kernel.org/doc/html/latest/input/uinput.html
- https://gitlab.freedesktop.org/libevdev/evtest
- https://github.com/torvalds/linux/blob/master/samples/hidraw/hid-example.c
- https://github.com/redmcg/FTE1001/blob/master/main.c
- https://github.com/torvalds/linux/blob/master/drivers/hid/hid-multitouch.c
- https://github.com/torvalds/linux/blob/master/drivers/input/input-mt.c
- http://bitmath.org/code/mtdev/
- https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/windows-precision-touchpad-required-hid-top-level-collections


