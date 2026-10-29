# BIYOS
BIYOS (Build It Yourself Operating System) is a small OS built entirely from scratch.
I am vibe coding it using Claude Sonnet 5.5 in C and Assembly.

# Filesystem
BIYOS uses FAT32 for the ESP boot partition and NTFS for everything else with a Windows-style C:\

# Functionality
It only has these things: a bootloader, a shell and essential drivers (USBHID, Video, etc.)
but you, the user can freely modify BIYOS from inside BIYOS using hexwrite and/or write and the shell command module, you can 
write direct x86_64 Assembly instructions after the header of a executable using write or hexwrite and then you can either run it using ./file or you can load executables as Kernel modules using module load filename.

# Executable design (still work in progress)
The executable header is defined like this:
hex code 0xAA
ASCII string EXE
executable type (can be STRT for a program that gets startet on boot if inside C:\Start, DRIV for drivers or PLSE if it should just be a standard program)
4 bytes for the 
executable version
8 bytes reserved for a smart comment or description (gets skipped by BIYOS)
and that's the standard-header but there can also be the ASCII string IMPORT after the first 20 bytes wich then is the beginning of the import table. That is the current executable design.

# RUN AT YOUR OWN RISK
This software is distributed "as-is", i am not responsible for any damages happening to your machine or data if you run malicious code or something similar.

# Distribution
You can freely make copies or modifications of this software and release them if you want. (I know these last two sections are also in the LICENSE file but i just also wrote it here just in case someone doesn't read the LICENSE file)
