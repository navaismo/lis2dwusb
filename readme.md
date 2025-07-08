# LIS2DWUSB

## Overview

This project provides the firmware for using a [BIGTREETECH S2DW](https://github.com/bigtreetech/LIS2DW/blob/master/BIGTREETECH%20S2DW%20V1.0%20User%20Manual_20250507.pdf) (RPi2040 based LIS2DW sensor module) and the Linux Wrapper to read the RP output to be used with [OctoPrint-Pinput\_Shaping](https://github.com/navaismo/Octoprint-Pinput_Shaping).

## Features

* Supports LIS2DW via RP2040 BTT-LIS2DW.
* Compatible with OctoPrint’s Pinput Shaping plugin.

## Requirements

* [BIGTREETECH S2DW](https://github.com/bigtreetech/LIS2DW/blob/master/BIGTREETECH%20S2DW%20V1.0%20User%20Manual_20250507.pdf)
* gcc 
* platformio.

## Installation

1. Compile(src_RP2040) or Download(FW_Binaries) the Firmware for the RPi2040.
2. Flash the RPi2040:
  - Hold the `BOOT` button from the BTT-LIS2DW board.
  - Connect the Board to your machine while keeping pressing the `BOOT` button.
  - Wait for your computer to show the new Storage Device and release the button.
  - Mount and Open the Device.
  - Copy the Downloaded `RP2040_LIS2DW_XXX.uf2` file (Or your compiled version `Firmware.uf2`) to the Device.
  - When it finish the copy the device will unmount automatically, means the RPi2040 is rebooting.
  - Disconnect the Device from your computer.
  - Connect the device to your Raspberry Pi running OctoPrint (or the device running OctoPrint).


3. Copy the `Linux_Wrapper` folder to any location on the OctoPrint-running device (Raspberry Pi).
4. Run the installation script with sudo.

   ```bash
   sudo bash ./install.sh
   ```



## Usage

* The `lis2dwusb` wrapper is installed in `/usr/local/bin/`.
* The Valid Frequencies for LIS2DW are from 200hz to 1600Hz.
* The OctoPrint user has direct permission to use this tool without sudo.
* In OctoPrint-Pinput\_Shaping, point the tool need to configure the option in Plugin Setting.
* To test from shell run:

```bash
sudo lis2dwusb -f 1600
```

or to Save it into a csv:
```bash
sudo lis2dwusb -f 1600 -s values.csv
```



## License

This project is open-source under the GPLv3 License.

## Contributing

Feel free to open issues or submit pull requests.
