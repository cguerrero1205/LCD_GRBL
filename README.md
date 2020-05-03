# 
This is an offline controller for a CNC machine with GRBL V1.1.

It executes the basic functions, such as:

Displaying the status of the machine. 

Move the axes. *Auto Home ($H)

Unlock grbl ($X) 
Make position 0 (G92 on all axes) 
Read a MicroSD and send the commands to the machine 
Among other options that will be added.

Components: 
-Arduino Mega. 
-Rotary encoder. 
-SPI microSD card reader. 
-LCD screen with i2c module.
