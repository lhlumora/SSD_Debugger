# SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import logging
import socket
import time

import netifaces
import pytest
from common_test_methods import get_env_config_variable
from common_test_methods import get_my_interface_by_dest_ip
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize

MESSAGE = 'Data to ESP'

# The three UARTs of the ESP32 have their own TCP server
PORTS = [3333, 3334, 3335]


def tcp_send(address: str, port: int, payload: str) -> None:
    """Connect to a server and send a payload.

    The server of this example does not echo the data any more: it writes it
    to the UART of the server. The test only checks that the connection is
    accepted and that the data can be sent.
    """
    for res in socket.getaddrinfo(address, port, socket.AF_UNSPEC,
                                  socket.SOCK_STREAM, 0, socket.AI_PASSIVE):
        family_addr, _, _, _, addr = res
    sock = socket.socket(family_addr, socket.SOCK_STREAM)
    sock.settimeout(60.0)
    try:
        sock.connect(addr)
    except socket.error as e:
        print('Could not open socket: ' + str(e))
        sock.close()
        raise
    sock.sendall(payload.encode())
    sock.close()


@pytest.mark.wifi_router
@pytest.mark.parametrize(
    'port',
    PORTS,
)
def test_examples_tcp_server_ipv4(dut: Dut, port: int) -> None:
    # Parse IP address of STA
    logging.info('Waiting to connect with AP')
    if dut.app.sdkconfig.get('EXAMPLE_WIFI_SSID_PWD_FROM_STDIN') is True:
        dut.expect('Please input ssid password:')
        env_name = 'wifi_router'
        ap_ssid = get_env_config_variable(env_name, 'ap_ssid')
        ap_password = get_env_config_variable(env_name, 'ap_password')
        dut.write(f'{ap_ssid} {ap_password}')
    ipv4 = dut.expect(r'IPv4 address: (\d+\.\d+\.\d+\.\d+)[^\d]', timeout=60)[1].decode()
    logging.info(f'Connected with IPv4={ipv4}')
    time.sleep(1)

    # test IPv4
    tcp_send(ipv4, port, MESSAGE)


@pytest.mark.wifi_router
@pytest.mark.xtal_26mhz
@pytest.mark.parametrize(
    'config, baud',
    [
        ('c2_xtal26m', '74880'),
    ],
    indirect=True,
)
@pytest.mark.parametrize(
    'port',
    PORTS,
)
def test_examples_tcp_server_ipv4_esp32c2_26mhz(dut: Dut, port: int) -> None:
    # Parse IP address of STA
    logging.info('Waiting to connect with AP')
    if dut.app.sdkconfig.get('EXAMPLE_WIFI_SSID_PWD_FROM_STDIN') is True:
        dut.expect('Please input ssid password:')
        env_name = 'wifi_router'
        ap_ssid = get_env_config_variable(env_name, 'ap_ssid')
        ap_password = get_env_config_variable(env_name, 'ap_password')
        dut.write(f'{ap_ssid} {ap_password}')
    ipv4 = dut.expect(r'IPv4 address: (\d+\.\d+\.\d+\.\d+)[^\d]', timeout=60)[1].decode()
    logging.info(f'Connected with IPv4={ipv4}')
    time.sleep(1)

    # test IPv4
    tcp_send(ipv4, port, MESSAGE)


@pytest.mark.wifi_router
@pytest.mark.parametrize(
    'port',
    PORTS,
)
def test_examples_tcp_server_ipv6(dut: Dut, port: int) -> None:
    # Parse IP address of STA
    logging.info('Waiting to connect with AP')
    if dut.app.sdkconfig.get('EXAMPLE_WIFI_SSID_PWD_FROM_STDIN') is True:
        dut.expect('Please input ssid password:')
        env_name = 'wifi_router'
        ap_ssid = get_env_config_variable(env_name, 'ap_ssid')
        ap_password = get_env_config_variable(env_name, 'ap_password')
        dut.write(f'{ap_ssid} {ap_password}')
    ipv4 = dut.expect(r'IPv4 address: (\d+\.\d+\.\d+\.\d+)[^\d]', timeout=60)[1].decode()
    # expect all 8 octets from IPv6 (assumes it's printed in the long form)
    ipv6_r = r':'.join((r'[0-9a-fA-F]{4}',) * 8)
    ipv6 = dut.expect(ipv6_r, timeout=60)[0].decode()
    logging.info(f'Connected with IPv4={ipv4} and IPv6={ipv6}')
    time.sleep(1)

    interface = get_my_interface_by_dest_ip(ipv4)
    # test IPv6
    tcp_send(f'{ipv6}%{interface}', port, MESSAGE)


@pytest.mark.wifi_router
@pytest.mark.parametrize(
    'port',
    PORTS,
)
def test_examples_tcp_server_ipv6_only(dut: Dut, port: int) -> None:
    # Parse IP address of STA
    logging.info('Waiting to connect with AP')
    if dut.app.sdkconfig.get('EXAMPLE_WIFI_SSID_PWD_FROM_STDIN') is True:
        dut.expect('Please input ssid password:')
        env_name = 'wifi_router'
        ap_ssid = get_env_config_variable(env_name, 'ap_ssid')
        ap_password = get_env_config_variable(env_name, 'ap_password')
        dut.write(f'{ap_ssid} {ap_password}')
    # expect all 8 octets from IPv6 (assumes it's printed in the long form)
    ipv6_r = r':'.join((r'[0-9a-fA-F]{4}',) * 8)
    ipv6 = dut.expect(ipv6_r, timeout=60)[0].decode()
    logging.info(f'Connected AP with IPv6={ipv6}')
    time.sleep(5)

    # test IPv6, try all interfaces
    for interface in netifaces.interfaces():
        try:
            logging.info(f'[{interface}] Connect to tcp_server {ipv6}%{interface}')
            tcp_send(f'{ipv6}%{interface}', port, MESSAGE)
        except Exception:
            # wrong interface
            continue
        break
    else:
        raise AssertionError(f'Can not connect to tcp_server: {ipv6}')
