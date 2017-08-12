## ELAN1200 touchpad driver
<br/>
A linux kernel module for ElanTech 1200 touchpad in Asus UX310UQ laptop. The way the default hid-multitouch driver reports touchpad's data results in random jumps of a cursor and random right-click events caused by the fake two-finger taps during two-finger scrolling. This driver somewhat minimises the glitches by filtering out the unrelated reports from the touchpad.
<br/><br/>
The repository also contains a userspace driver, based on https://github.com/redmcg/FTE1001 which can be used for debugging the data from a hidraw device.
<br/><br/>

The driver is tested on Debian 9, kernel 4.11.

### Problem report
https://bugzilla.kernel.org/show_bug.cgi?id=196619
