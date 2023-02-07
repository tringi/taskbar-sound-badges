# Taskbar Sound Badges
*Small program that monitors which applications are currently playing sound, and sets Taskbar Badge on icons of those that do.*

**This is a preview version.**

## Installation and configuration

No installer is currently provided. Build and copy ALL EXE/DLL files where appropriate (typically Program Files) and start the right one:
* TbSndBg32.exe on 32-bit Windows
* TbSndBg64.exe on 64-bit Windows
* and TbSngBgAA.exe on ARM64 Windows.

Note that executables for other architectures are still required to be present.  
Starting a wrong executable may start, but will not work properly.

Make sure to have Taskbar Badges enabled in Settings.

The program adds Notification Area (tray) icon through which some functionality is accessed and the program can be exited.

## Requirements

* Windows 7 or later

## Preview version caveats

* The program will override Taskbar Badges if the application already uses them and makes sound
* Applications that defer audio playback to other processes are not recognized, e.g.: Chromium-based browsers
* Badge style (changed from tray menu) is not remembered after restart

## Download

Prebuild executables can be downloaded from: http://tringi.trimcore.cz/Taskbar_Sound_Badges
