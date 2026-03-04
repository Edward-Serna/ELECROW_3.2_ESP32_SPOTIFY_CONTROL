#pragma once

#define ILI9488_DRIVER

// Backlight (critical on many of these boards)
#define TFT_BL 27
#define TFT_BACKLIGHT_ON HIGH

// SPI pins used by CrowPanel-style Elecrow 3.5"
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_MISO 33   // v2.2
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1

// Touch controller CS (XPT2046)
#define TOUCH_CS 5   // v2.2

#define SPI_FREQUENCY        27000000
#define SPI_READ_FREQUENCY   16000000
#define SPI_TOUCH_FREQUENCY   2500000

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define SMOOTH_FONT

#define JPEG_DECODER