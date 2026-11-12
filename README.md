ESP DECK — Required Libraries, Software & Hardware

1. Required Libraries

The ESP DECK firmware uses the following libraries:

Library| Purpose
Adafruit GFX Library| Graphics, text, shapes and drawing
Adafruit ST7735 and ST7789 Library| Controls the TFT display
ArduinoJson (Version 7 recommended)| Processes online JSON data
SPI| Communication with the TFT and SD card
SD| Reads and writes files on the SD card
WiFi| Connects the ESP32-C3 to Wi-Fi
WiFiClientSecure| Secure HTTPS connections
HTTPClient| Sends internet requests

Libraries that need to be installed

Only these three external libraries need to be installed separately:

1. Adafruit GFX Library
2. Adafruit ST7735 and ST7789 Library
3. ArduinoJson 7

The remaining libraries are included with the compatible ESP32 Arduino core or the standard toolchain.

Official library sources:

- Adafruit GFX: https://github.com/adafruit/Adafruit-GFX-Library
- Adafruit ST7735: https://github.com/adafruit/Adafruit-ST7735-Library
- ArduinoJson: https://arduinojson.org/

2. Required Board

Main microcontroller:

- ESP32-C3 SuperMini
- Built-in Wi-Fi
- Limited RAM, so memory-efficient firmware is important

3. Required Hardware

Component| Purpose
ESP32-C3 SuperMini| Main controller
1.8-inch ST7735 TFT| Main display (160 × 128)
MicroSD card module / TFT SD slot| Local file storage
4 tactile buttons| User controls
Jumper wires| Connections
Power bank or USB power supply| Power
Enclosure| Cyberdeck body

GPS is not required. Maps and weather features use manually selected locations and internet services.

4. Pin Configuration

Four physical buttons

Button| ESP32-C3 GPIO
LEFT| GPIO 21
RIGHT| GPIO 20
A| GPIO 10
B| GPIO 9

TFT display

TFT pin| ESP32-C3 GPIO
CS| GPIO 4
DC / A0| GPIO 3
RESET| GPIO 8
SCK| GPIO 1
MOSI / SDA| GPIO 0
VCC| 3.3V
LED| 3.3V
GND| GND

SD card

SD pin| ESP32-C3 GPIO
CS| GPIO 6
MOSI| GPIO 5
MISO| GPIO 7
SCK| GPIO 2

Important: Check the actual board pin labels and wiring before powering the device. Do not assume every ESP32-C3 SuperMini board has identical physical pin placement.

5. Internet Services

Some ESP DECK features require an internet connection:

- ntfy: Internet messaging.
- Open-Meteo: Weather information.
- OpenStreetMap services: Place search and map-related information.
- Wikipedia: Online encyclopedia access.

These services may have their own availability limits and network requirements.

6. Before Building the Firmware

1. Select a compatible ESP32-C3 board configuration.
2. Install the three external libraries listed above.
3. Check the GPIO configuration.
4. Connect the TFT and SD card correctly.
5. Connect the ESP32-C3 to your computer or supported programming device.
6. Compile the complete firmware.
7. Upload it to the ESP32-C3.
8. Restart the device and test its features.

Keep a backup of your working firmware before uploading a new version.

---

Note: ESP DECK is designed around a specific ESP32-C3 SuperMini and display configuration. Changing the board, display controller, GPIO assignments or library versions may require code modifications.