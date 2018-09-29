## ELAN1200 touchpad driver
<br/>
A linux kernel module for ElanTech 1200 touchpad in Asus UX310UQ laptop. The way the default hid-multitouch driver reports touchpad's data results in random jumps of a cursor and random right-click events caused by the fake two-finger taps during two-finger scrolling. This driver somewhat minimises the glitches by filtering out the unrelated reports from the touchpad.
<br/><br/>
The repository also contains a userspace driver, based on https://github.com/redmcg/FTE1001 which can be used for debugging the data from a hidraw device.
<br/><br/>

The driver is tested on Debian 9, kernels 4.11, 4.13, 4.14, 4.17

In 4.14 the API for timers has changed, the code in the latest commits is incompatible with the kernels less than 4.14. There's the tag, "4.13", for the older versions.

## Warning
If the driver doesn't work well. There's a patch (drop_releases.diff) for xorg synaptics driver 1.9.0 and 1.9.1 which implements the same idea: it drops the artificial releases if a next report is a touch, it happens when the touchpad starts to report two close contacts as one wide contact. Probably to handle those reports in user space is more natural from the architectural point of view.

### Problem report
https://bugzilla.kernel.org/show_bug.cgi?id=196619


## PS
![ScreenShot](http://mishurov.co.uk/images/github/linux_elan1200_touchpad/pm.png)
<br/><br/>
There's also an ACPI problem related to this family of ASUS laptops. When a battery is fully charged, it goes into the state of discharging at zero rate. I made a patch (pm_asus_patch.diff) for xfce4-power-manager-1.6.1 which shows a correct icon when an AC cable is plugged in. It may be useful for someone.

