| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- | -------- | -------- |


# TCP Server example

(See the README.md file in the upper level 'examples' directory for more information about examples.)

The application creates a TCP socket with the specified port number and waits for a connection request from the client. After accepting a request from the client, connection between server and client is established and the application waits for some data to be received from the client. Received data are printed as ASCII text and retransmitted back to the client.

## Interactive console

The Wi-Fi connection and the TCP servers are **not** started automatically. Both are
controlled from an interactive console (ESP-IDF `console` component), so the AP and
the servers can be selected at runtime.

| Command | Description |
| ------- | ----------- |
| `scan` | Scan the surrounding APs and print them with an index |
| `connect <index\|ssid> [password]` | Connect to an AP (the password is asked interactively if the AP is protected and no password was given) |
| `disconnect` | Leave the current AP |
| `wifi_pass [password\|clear]` | Set / clear the default password used by `connect` |
| `status` | Show the Wi-Fi state, the IP address, the state of the UART servers and the status LED |
| `server_start [uartN\|all]` | Start the TCP server of one UART or of all of them |
| `server_stop [uartN\|all]` | Stop the TCP server of one UART or of all of them |
| `uart_baud <uartN> <baud>` | Set the baud rate of a UART |
| `uart_pins <uartN> <tx> <rx>` | Set the GPIO pins of a UART |
| `tasks` | List the FreeRTOS tasks (state, priority, core, free stack, run time) |
| `mem` | Show the memory usage (internal RAM, DMA, PSRAM) |
| `cpu` | Show the CPU load, measured over one second |
| `reboot` | Restart the ESP32-S3 (the TCP servers are stopped and the AP is left first) |
| `lcd_info` | Show the settings of the display |
| `lcd_backlight [on\|off]` | Switch the backlight of the display on or off |
| `lcd_rotate [degrees]` | Rotate the screen of the display |
| `help [command]` | List all the commands, or show the details of one command |

Example session:

```
esp32s3> scan

 Idx  SSID                          RSSI    CH  AUTH
   1  DPU_LocalNet                    -45     6  WPA2_PSK
   2  MyGuest                          -72    11  OPEN

esp32s3> connect 1
Password: ********
Connecting to 'DPU_LocalNet'...
Got IP address: 192.168.0.167
Connected, IP address: 192.168.0.167

esp32s3> server_start uart0
TCP server of UART0 started on port 3333 (192.168.0.167).

esp32s3> status
Wi-Fi     : connected
IP address: 192.168.0.167
TCP server: 1 running (port 3333)
LED       : server running

UART bridges (rx: TCP->UART, tx: UART->TCP):
  UART0  port 3333  running  connected  rx 0 B       tx 0 B
  UART1  port 3334  stopped  no client  rx 0 B       tx 0 B
  UART2  port 3335  stopped  no client  rx 0 B       tx 0 B
```

## UART servers

The three UART peripherals of the ESP32 are used to exchange data with the
outside world. Each of them has its own TCP server:

| UART | TCP port | Default pins (TX / RX) |
| ---- | -------- | ---------------------- |
| UART0 | 3333 | GPIO43 / GPIO44 |
| UART1 | 3334 | GPIO17 / GPIO18 |
| UART2 | 3335 | GPIO15 / GPIO16 |

The port of a UART is `CONFIG_EXAMPLE_PORT` plus the number of the UART. The
ports and the GPIO pins can be changed in the menuconfig (*Example
Configuration Options*), the baud rate (`CONFIG_EXAMPLE_UART_BAUD`, 115200 by
default) too.

The data flows in both directions:

* the data received from the client is written to the UART,
* the data received from the UART is sent to the client.

The data received from a UART while no client is connected is dropped.

The drivers of the UARTs are installed only when their server is started, so
the GPIOs of the UARTs that are not used stay available. This makes it possible
to run only the servers that are really needed:

```
esp32s3> server_start uart0      # only UART0 and the port 3333
esp32s3> server_start all        # the three UARTs and the ports 3333-3335
esp32s3> server_stop uart1       # stop the server of UART1 only
esp32s3> server_stop all         # stop the three servers
```

Notes:

* The console port is the one selected in menuconfig (`Component config ->
  ESP System Settings -> Channel for console output`). The default of this
  project is the USB-Serial-JTAG port of the ESP32-S3.
* The password typed on the console is echoed by the terminal, the ESP-IDF
  console has no way of hiding it.
* The log output and the REPL prompt share the same port, so the log messages
  may appear in between the prompt lines.
* The `tasks` and `cpu` commands need the FreeRTOS run time stats, they are
  enabled in `sdkconfig` (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`).
* Both cores of the ESP32-S3 run FreeRTOS. The tasks are not pinned, so they
  can migrate from one core to the other. The `Core` column of the `tasks`
  command shows the core a task is running on (`*`), the core it is pinned to
  (a plain number) or `-` when it is not pinned at all.
* The GPIOs 0, 3, 45 and 46 are strapping pins of the ESP32-S3, they should
  not be used by the UARTs.

## Status LED

A status LED can be attached to a GPIO (GPIO21 by default, see menuconfig ->
Example Configuration Options -> "Status LED GPIO number"). The LED is active
low: it is lit while the pin is driven low. A dedicated task drives it, the
pattern shows the state of the system:

| Pattern | State |
| ------- | ----- |
| Fast blink | The system is starting up |
| Slow blink | Not connected to an AP |
| Solid on | Connected to an AP, the TCP server is stopped |
| Double blink | The TCP server is running |
| Rapid blink | Error |

The state currently shown is printed by the `status` command.

## Display (ST7735 + LVGL)

A ST7735 display (1.8 inch, 128x160 pixels) can be attached to the SPI bus
of the board. It is driven by [LVGL](https://lvgl.io/) and shows a dashboard of
the system: the state of the Wi-Fi connection, the IP address, the TCP servers,
the CPU load, the free memory, the running time and the state of the status
LED. The dot in the title bar shows the state of the status LED.

The wiring of the display (7 pins, the module has also a VCC and a GND pin):

| Signal | Description | Default GPIO |
| ------ | ----------- | ------------ |
| SCL    | SPI clock | 43 |
| SDA    | SPI data (MOSI) | 44 |
| CS     | Chip select | 8 |
| DC     | Data / command | 7 |
| RES    | Reset | 6 |
| BL     | Backlight | 9 |

All the pins are configurable in menuconfig -> Example Configuration Options ->
Display (ST7735). The backlight can be switched off when it is not connected
to a GPIO (set its number to -1), the polarity of the backlight can be
inverted as well.

The settings of the panel (size, orientation, offset of the visible area in
the memory of the controller, color order) are in the same menu. The defaults
are the ones of a 1.8 inch 128x160 module. The 0.96 inch 80x160 modules need
a width of 80 and an offset of 26/1 (green) or 24/0 (red).

The dashboard is refreshed every second. The display can be controlled from the
console:

| Command | Description |
| ------- | ----------- |
| `lcd_info` | Show the settings of the display |
| `lcd_backlight [on\|off]` | Switch the backlight on or off |
| `lcd_rotate [degrees]` | Rotate the screen (0, 90, 180 or 270) |
| `lcd_refresh` | Draw the dashboard again over the whole screen |
| `lcd_test [raw\|map\|pattern\|color]` | Draw a test pattern on the screen |
| `lcd_offset [col row]` | Show or set the offset of the visible area |
| `lcd_madctl [value]` | Show or set the MADCTL register of the controller |
| `lcd_clock [hz]` | Show or set the speed of the SPI bus |

Notes:

* The screen uses the SPI2 peripheral, the other peripherals of the board
  (the three UARTs) are not affected.
* The partition table of the project was enlarged (`partitions.csv`), the
  LVGL library needs about 400 kB of flash.
* The configuration of LVGL is in `main/lv_conf.h`: the color format, the
  memory allocator, the fonts and the widgets that are compiled in.

## How to use example

In order to create TCP client that communicates with TCP server example, choose one of the following options.

There are many host-side tools which can be used to interact with the UDP/TCP server/client.
One command line tool is [netcat](http://netcat.sourceforge.net) which can send and receive many kinds of packets.
Note: please replace `192.168.0.167 3333` with desired IPV4/IPV6 address (displayed in monitor console) and port number in the following command.

Ref to the [upper level README](../README.md#host-tools) for more information.

### TCP client using netcat
```
nc 192.168.0.167 3333
```

## Hardware Required

This example can be run on any commonly available ESP32 development board.

## Configure the project

```
idf.py menuconfig
```

Set following parameters under Example Configuration Options:

* Set `IP version` of the example to be IPV4 or IPV6.

* Set `Port` number of the socket, that server example will create. The
  servers of UART1 and UART2 use the next ports.

* Set the `UART baud rate` and the `UART0/1/2 TX/RX GPIO` values of the
  three UART peripherals.

* Set `TCP keep-alive idle time(s)` value of TCP keep alive idle time. This time is the time between the last data transmission.

* Set `TCP keep-alive interval time(s)` value of TCP keep alive interval time. This time is the interval time of keepalive probe packets.

* Set `TCP keep-alive packet retry send counts` value of TCP keep alive packet retry send counts. This is the number of retries of the keepalive probe packet.

Configure Wi-Fi or Ethernet under "Example Connection Configuration" menu. See "Establishing Wi-Fi or Ethernet Connection" section in [examples/protocols/README.md](../../README.md) for more details.

## Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.


## Troubleshooting

Start server first, to receive data sent from the client (application).
