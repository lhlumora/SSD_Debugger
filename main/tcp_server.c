/*
 * TCP servers
 *
 * One TCP server per UART peripheral, the data received from the client is
 * written to the UART and the data received from the UART is sent to the
 * client. See tcp_server.h for the description of the module.
 */
#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "tcp_server.h"
#include "uart_bridge.h"

#define PORT_BASE                  CONFIG_EXAMPLE_PORT
#define KEEPALIVE_IDLE             CONFIG_EXAMPLE_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL         CONFIG_EXAMPLE_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT            CONFIG_EXAMPLE_KEEPALIVE_COUNT

/* How long tcp_server_stop() waits for the task to finish */
#define SERVER_STOP_TIMEOUT_MS     3000
/* How often the server task wakes up to look at the UART and at the stop
   request. It is also the maximum delay between the reception of a byte on
   the UART and its transmission to the client. */
#define SERVER_POLL_MS             20
/* Size of the buffers used to transfer the data between the UART and the
   socket */
#define SERVER_BUFFER_SIZE         256
/* Timeout of the send and of the receive operations of a client socket */
#define SERVER_SOCKET_TIMEOUT_S    2

static const char *TAG = "tcp_server";

/* One TCP server */
typedef struct {
    int uart_num;               /* UART peripheral of this server */
    int port;                   /* TCP port of this server */
    volatile bool running;      /* the server has been started */
    volatile int active_tasks;  /* number of running tasks */
    volatile bool client;       /* a client is connected */
    volatile uint32_t rx_bytes; /* bytes received from the client */
    volatile uint32_t tx_bytes; /* bytes sent to the client */
    volatile uint32_t dropped_bytes; /* bytes received from the UART with no client */
} server_t;

static server_t s_servers[UART_BRIDGE_COUNT] = {
    { 0, PORT_BASE + 0, false, 0, false, 0, 0, 0 },
    { 1, PORT_BASE + 1, false, 0, false, 0, 0, 0 },
    { 2, PORT_BASE + 2, false, 0, false, 0, 0, 0 },
};

/* The address family of the listening sockets. The example is built for one
   family at a time, IPv4 is preferred when both are available. */
#if defined(CONFIG_EXAMPLE_IPV4)
#define SERVER_ADDR_FAMILY        AF_INET
#elif defined(CONFIG_EXAMPLE_IPV6)
#define SERVER_ADDR_FAMILY        AF_INET6
#else
#error "Either CONFIG_EXAMPLE_IPV4 or CONFIG_EXAMPLE_IPV6 must be enabled"
#endif

static void tcp_server_task(void *pvParameters);

esp_err_t tcp_server_start(int uart_num)
{
    if (uart_num < 0 || uart_num >= UART_BRIDGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    server_t *srv = &s_servers[uart_num];
    if (srv->running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The UART is needed by the server, install its driver if the console
       did not do it already. */
    if (!uart_bridge_is_open(uart_num)) {
        esp_err_t err = uart_bridge_open(uart_num);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Unable to open UART%d: %s", uart_num, esp_err_to_name(err));
            return err;
        }
    }

    srv->running = true;
    srv->rx_bytes = 0;
    srv->tx_bytes = 0;
    srv->dropped_bytes = 0;
    srv->client = false;

    /* The counter is incremented before the task is created, so that the
       task cannot decrement it before the increment happens. */
    srv->active_tasks++;
    char task_name[16];
    snprintf(task_name, sizeof(task_name), "tcp_uart%d", uart_num);
    if (xTaskCreate(tcp_server_task, task_name, 4096, (void *)(intptr_t)uart_num, 5, NULL) != pdTRUE) {
        srv->active_tasks--;
        srv->running = false;
        ESP_LOGE(TAG, "Failed to create the TCP server task of UART%d", uart_num);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TCP server of UART%d started on port %d", uart_num, srv->port);
    return ESP_OK;
}

esp_err_t tcp_server_stop(int uart_num)
{
    if (uart_num < 0 || uart_num >= UART_BRIDGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    server_t *srv = &s_servers[uart_num];
    if (!srv->running) {
        return ESP_ERR_INVALID_STATE;
    }

    srv->running = false;

    /* Wait until the task has closed its sockets and exited */
    for (int waited = 0; waited < SERVER_STOP_TIMEOUT_MS; waited += 50) {
        if (srv->active_tasks == 0) {
            /* The UART is not used any more, give its GPIOs back */
            uart_bridge_close(uart_num);
            ESP_LOGI(TAG, "TCP server of UART%d stopped", uart_num);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGE(TAG, "TCP server of UART%d did not stop within %d ms", uart_num,
             SERVER_STOP_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

esp_err_t tcp_server_start_all(void)
{
    int started = 0;

    for (int i = 0; i < UART_BRIDGE_COUNT; i++) {
        esp_err_t err = tcp_server_start(i);
        if (err == ESP_OK) {
            started++;
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Unable to start the TCP server of UART%d: %s", i,
                     esp_err_to_name(err));
        }
    }

    return (started > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t tcp_server_stop_all(void)
{
    int stopped = 0;

    for (int i = 0; i < UART_BRIDGE_COUNT; i++) {
        esp_err_t err = tcp_server_stop(i);
        if (err == ESP_OK) {
            stopped++;
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Unable to stop the TCP server of UART%d: %s", i,
                     esp_err_to_name(err));
        }
    }

    return (stopped > 0) ? ESP_OK : ESP_FAIL;
}

bool tcp_server_is_running(int uart_num)
{
    if (uart_num < 0 || uart_num >= UART_BRIDGE_COUNT) {
        return false;
    }
    return s_servers[uart_num].running;
}

bool tcp_server_any_running(void)
{
    for (int i = 0; i < UART_BRIDGE_COUNT; i++) {
        if (s_servers[i].running) {
            return true;
        }
    }
    return false;
}

int tcp_server_port(int uart_num)
{
    if (uart_num < 0 || uart_num >= UART_BRIDGE_COUNT) {
        return 0;
    }
    return s_servers[uart_num].port;
}

void tcp_server_get_stats(int uart_num, tcp_server_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    if (uart_num < 0 || uart_num >= UART_BRIDGE_COUNT) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    const server_t *srv = &s_servers[uart_num];
    stats->port = srv->port;
    stats->running = srv->running;
    stats->client = srv->client;
    stats->rx_bytes = srv->rx_bytes;
    stats->tx_bytes = srv->tx_bytes;
    stats->dropped_bytes = srv->dropped_bytes;
}

/* Send data to the socket, returns the number of bytes that were sent.
   A timeout (the client does not read fast enough) is not an error: the data
   that could not be sent is dropped. A negative value means that the
   connection is broken. */
static int send_data(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        int written = send(sock, data + sent, len - sent, 0);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* The client does not read fast enough, drop the rest */
                break;
            }
            return -1;
        }
        sent += (size_t)written;
    }
    return (int)sent;
}

static void tcp_server_task(void *pvParameters)
{
    const int uart_num = (int)(intptr_t)pvParameters;
    server_t *srv = &s_servers[uart_num];
    char addr_str[128];
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;
    uint8_t rx_buffer[SERVER_BUFFER_SIZE];
    uint8_t tx_buffer[SERVER_BUFFER_SIZE];

    memset(&dest_addr, 0, sizeof(dest_addr));
#if SERVER_ADDR_FAMILY == AF_INET
    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(srv->port);
    const int ip_protocol = IPPROTO_IP;
#else
    struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
    dest_addr_ip6->sin6_addr.un.in6_addr = in6addr_any;
    dest_addr_ip6->sin6_family = AF_INET6;
    dest_addr_ip6->sin6_port = htons(srv->port);
    const int ip_protocol = IPPROTO_IPV6;
#endif

    const int listen_sock = socket(SERVER_ADDR_FAMILY, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        goto CLEAN_UP;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "Socket created for UART%d", uart_num);

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind on port %d: errno %d", srv->port, errno);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", srv->port);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    ESP_LOGI(TAG, "UART%d server listening, waiting for a client on port %d",
             uart_num, srv->port);

    int client_sock = -1;

    while (srv->running) {
        /* Wait for a client or for the data of the UART. The timeout of the
           select() keeps the task responsive to the stop request and lets it
           forward the data received on the UART to the client. */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_sock, &read_fds);
        int max_fd = listen_sock;

        if (client_sock >= 0) {
            FD_SET(client_sock, &read_fds);
            if (client_sock > max_fd) {
                max_fd = client_sock;
            }
        }

        struct timeval poll_timeout = {
            .tv_sec = 0,
            .tv_usec = SERVER_POLL_MS * 1000,
        };

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &poll_timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGE(TAG, "Error occurred during select: errno %d", errno);
            break;
        }

        /* A client wants to connect */
        if (client_sock < 0 && FD_ISSET(listen_sock, &read_fds)) {
            struct sockaddr_storage source_addr; /* IPv4 or IPv6 */
            socklen_t addr_len = sizeof(source_addr);
            int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
            if (sock < 0) {
                ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
                continue;
            }

            /* Set tcp keepalive option */
            setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
            setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

            /* Timeouts of the socket, a client that does not read must not
               block the server forever. */
            struct timeval socket_timeout = {
                .tv_sec = SERVER_SOCKET_TIMEOUT_S,
                .tv_usec = 0,
            };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout, sizeof(socket_timeout));

            /* Convert ip address to string */
#if SERVER_ADDR_FAMILY == AF_INET
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str,
                        sizeof(addr_str) - 1);
#else
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str,
                         sizeof(addr_str) - 1);
#endif
            ESP_LOGI(TAG, "Client %s connected to the UART%d server (port %d)",
                     addr_str, uart_num, srv->port);

            client_sock = sock;
            srv->client = true;
        }

        /* Data received from the client -> the UART */
        if (client_sock >= 0 && FD_ISSET(client_sock, &read_fds)) {
            int len = recv(client_sock, rx_buffer, sizeof(rx_buffer), 0);
            if (len > 0) {
                srv->rx_bytes += (uint32_t)len;
                int written = uart_bridge_write(uart_num, rx_buffer, (size_t)len);
                if (written < 0) {
                    written = 0;
                }
                /* The UART was busy, the rest of the data is dropped */
                srv->dropped_bytes += (uint32_t)len - (uint32_t)written;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Client disconnected from the UART%d server", uart_num);
                close(client_sock);
                client_sock = -1;
                srv->client = false;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
                close(client_sock);
                client_sock = -1;
                srv->client = false;
            }
        }

        /* Data received from the UART -> the client */
        int len = uart_bridge_read(uart_num, tx_buffer, sizeof(tx_buffer));
        if (len > 0) {
            if (client_sock >= 0) {
                int sent = send_data(client_sock, tx_buffer, (size_t)len);
                if (sent < 0) {
                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                    close(client_sock);
                    client_sock = -1;
                    srv->client = false;
                } else {
                    srv->tx_bytes += (uint32_t)sent;
                    /* The client was too slow, the rest of the data is dropped */
                    srv->dropped_bytes += (uint32_t)len - (uint32_t)sent;
                }
            } else {
                /* Nobody is listening, the data is dropped */
                srv->dropped_bytes += (uint32_t)len;
            }
        }
    }

    if (client_sock >= 0) {
        close(client_sock);
        srv->client = false;
    }
    shutdown(listen_sock, 0);

CLEAN_UP:
    if (listen_sock >= 0) {
        close(listen_sock);
    }
    srv->active_tasks--;
    vTaskDelete(NULL);
}
