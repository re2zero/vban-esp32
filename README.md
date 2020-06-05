
# Vban UDP protocol ESP32 implementation 

Refer the https://github.com/quiniouben/vban.git implementation.

Specification https://www.vb-audio.com/Voicemeeter/VBANProtocol_Specifications.pdf

## How to use example

### Send UDP packet via vban
```
vban_cfg.type = AUDIO_STREAM_WRITER;
audio_element_set_uri(vban_stream_reader, "192.168.0.167:6980");
```

### Receive UDP packet via vban
```
vban_cfg.type = AUDIO_STREAM_READER;
audio_element_set_uri(vban_stream_reader, "0.0.0.0:6980");
```
## Hardware Required

This example can be run on any commonly available ESP32 development board.

## Configure the project

```
make menuconfig
```

Set following parameter under Serial Flasher Options:

* Set `Default serial port`.

Set following parameters under Example Configuration Options:

* Set `WiFi SSID` of the Router (Access-Point).

* Set `WiFi Password` of the Router (Access-Point).

* Set `IP version` of the example to be IPV4 or IPV6.

* Set `Port` number that represents remote port the example will create.

## Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
make -j4 flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

