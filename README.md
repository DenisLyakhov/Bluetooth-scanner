# Bluetooth Low Energy devices scanner

Bluetooth scanner implementation that detects nearby devices with a Bluetooth Low Energy (BLE) interface using the ESP-IDF framework operating on the ESP32 microcontroller.

## Demonstration

![scanner_interface](https://github.com/user-attachments/assets/a5ad76cd-3545-4faa-9177-366da4b7d663)

## Installation

Libraries for ESP-EDF framework:

```
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-setuptools cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

Downloading ESP-EDF framework:

```
git clone --recursive https://github.com/espressif/esp-idf.git
```

Framework configuration:

```
~/esp/esp-idf/install.sh esp32
```

Setting environmental variables and aliases:

```
. $HOME/esp/esp-idf/export.sh
```

```
alias get_idf='. $HOME/esp/esp-idf/export.sh'

```


Setting target project (from inside the project folder):

```
idf.py set-target esp32
```

Build project:

```
idf.py build
```

Flashing the application on the connected ESP32 microcontroller:

```
idf.py -p <PORT_NUMBER> monitor
```

Monitor the scanning process:

```
idf.py -p <PORT_NUMBER> monitor
```

 
