| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- | -------- | -------- |


# TCP Server example

(See the README.md file in the upper level 'examples' directory for more information about examples.)

The application creates a TCP socket with the specified port number and waits for a connection request from the client. After accepting a request from the client, connection between server and client is established and the application waits for some data to be received from the client. Received data are printed as ASCII text and retransmitted back to the client.

## Interactive console

The Wi-Fi connection and the TCP server are **not** started automatically. Both are
controlled from an interactive console (ESP-IDF `console` component), so the AP and
the server can be selected at runtime.

| Command | Description |
| ------- | ----------- |
| `scan` | Scan the surrounding APs and print them with an index |
| `connect <index\|ssid> [password]` | Connect to an AP (the password is asked interactively if the AP is protected and no password was given) |
| `disconnect` | Leave the current AP |
| `wifi_pass [password\|clear]` | Set / clear the default password used by `connect` |
| `status` | Show the Wi-Fi state, the IP address, the TCP server state and the status LED |
| `server_start` | Start the TCP server (listens on `CONFIG_EXAMPLE_PORT`, 3333 by default) |
| `server_stop` | Stop the TCP server |
| `tasks` | List the FreeRTOS tasks (state, priority, core, free stack, run time) |
| `mem` | Show the memory usage (internal RAM, DMA, PSRAM) |
| `cpu` | Show the CPU load, measured over one second |
| `help` | List all commands (built-in) |

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

esp32s3> server_start
Starting TCP server on port 3333 (192.168.0.167)
TCP server started.

esp32s3> status
Wi-Fi     : connected
IP address: 192.168.0.167
TCP server: running (port 3333)
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

* Set `Port` number of the socket, that server example will create.

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
