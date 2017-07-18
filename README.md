## ELAN1200 touchpad driver
<br/>
A linux kernel module for a ElanTech touchpad. The default hid-multitouch driver doesn't report width/height data from the touchpad and reports some fake events, it results in random jumps of a cursor and random right-click events during two-finger scrolling. This driver somewhat minimises the glitches.
<br/><br/>
The repository also contains a userspace driver, based on https://github.com/redmcg/FTE1001 which can be used for debugging the data from a hidraw device.
<br/><br/>

### Problem report
https://bugzilla.kernel.org/show_bug.cgi?id=120181
