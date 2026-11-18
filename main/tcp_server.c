/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
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

#define PORT                        CONFIG_EXAMPLE_PORT
#define KEEPALIVE_IDLE              CONFIG_EXAMPLE_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL          CONFIG_EXAMPLE_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT             CONFIG_EXAMPLE_KEEPALIVE_COUNT

/* How long tcp_server_stop() waits for the task to finish */
#define SERVER_STOP_TIMEOUT_MS      3000
/* Poll interval used while waiting for an incoming connection.
   The task sleeps in select() during this time, so it does not consume CPU.
   It also defines how fast 'server_stop' is noticed. */
#define SERVER_ACCEPT_POLL_MS       1000
/* Receive timeout of an accepted socket, keeps the stop command responsive */
#define SERVER_RECV_TIMEOUT_S       1

static const char *TAG = "example";

/* Requested state (set by the console commands) */
static volatile bool s_server_running = false;
/* Number of tasks that are really running */
static volatile int  s_active_tasks = 0;

int tcp_server_port(void)
{
    return PORT;
}

bool tcp_server_is_running(void)
{
    return s_active_tasks > 0;
}

static void do_retransmit(const int sock)
{
    int len;
    char rx_buffer[128];

    while (s_server_running) {
        len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* receive timeout, just go back and check s_server_running */
                continue;
            }
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
            return;
        } else if (len == 0) {
            ESP_LOGW(TAG, "Connection closed");
            return;
        } else {
            rx_buffer[len] = 0; // Null-terminate whatever is received and treat it like a string
            ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);

            // send() can return less bytes than supplied length.
            // Walk-around for robust implementation.
            int to_write = len;
            while (to_write > 0) {
                int written = send(sock, rx_buffer + (len - to_write), to_write, 0);
                if (written < 0) {
                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                    // Failed to retransmit, giving up
                    return;
                }
                to_write -= written;
            }
        }
    }
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

#ifdef CONFIG_EXAMPLE_IPV4
    if (addr_family == AF_INET) {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;
    }
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    if (addr_family == AF_INET6) {
        struct sockaddr_in6 *dest_addr_ip6 = (struct sockaddr_in6 *)&dest_addr;
        bzero(&dest_addr_ip6->sin6_addr.un, sizeof(dest_addr_ip6->sin6_addr.un));
        dest_addr_ip6->sin6_family = AF_INET6;
        dest_addr_ip6->sin6_port = htons(PORT);
        ip_protocol = IPPROTO_IPV6;
    }
#endif

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        goto CLEAN_UP;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
    // Note that by default IPV6 binds to both protocols, it is must be disabled
    // if both protocols used at the same time (used in CI)
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
#endif

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    ESP_LOGI(TAG, "Socket listening, waiting for a client on port %d", PORT);

    while (s_server_running) {

        /* lwIP's accept() is blocking, so wait for the connection with a
           select() that has a timeout. This way 'server_stop' is handled
           within SERVER_ACCEPT_POLL_MS.
           The task sleeps inside select(), it does not spin. */
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_sock, &read_fds);
        struct timeval poll_timeout = {
            .tv_sec = 0,
            .tv_usec = SERVER_ACCEPT_POLL_MS * 1000,
        };

        int ready = select(listen_sock + 1, &read_fds, NULL, NULL, &poll_timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGE(TAG, "Error occurred during select: errno %d", errno);
            break;
        }
        if (ready == 0) {
            /* timeout, loop back and check s_server_running */
            continue;
        }

        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        /* Set tcp keepalive option */
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

        /* Receive timeout, so that a running client does not block the
           'server_stop' command forever. */
        struct timeval recv_timeout = {
            .tv_sec = SERVER_RECV_TIMEOUT_S,
            .tv_usec = 0,
        };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

        // Convert ip address to string
#ifdef CONFIG_EXAMPLE_IPV4
        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
#ifdef CONFIG_EXAMPLE_IPV6
        if (source_addr.ss_family == PF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
#endif
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        do_retransmit(sock);

        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    if (listen_sock >= 0) {
        close(listen_sock);
    }
    s_active_tasks--;
    vTaskDelete(NULL);
}

esp_err_t tcp_server_start(void)
{
    if (s_server_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_server_running = true;

    int created = 0;
#ifdef CONFIG_EXAMPLE_IPV4
    /* The counter is incremented before the task is created, so that the
       task cannot decrement it before the increment happens. */
    s_active_tasks++;
    if (xTaskCreate(tcp_server_task, "tcp_server", 4096, (void *)AF_INET, 5, NULL) == pdTRUE) {
        created++;
    } else {
        s_active_tasks--;
    }
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    s_active_tasks++;
    if (xTaskCreate(tcp_server_task, "tcp_server", 4096, (void *)AF_INET6, 5, NULL) == pdTRUE) {
        created++;
    } else {
        s_active_tasks--;
    }
#endif

    if (created == 0) {
        s_server_running = false;
        ESP_LOGE(TAG, "Failed to create the TCP server task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TCP server task started on port %d", PORT);
    return ESP_OK;
}

esp_err_t tcp_server_stop(void)
{
    if (!s_server_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_server_running = false;

    /* Wait until the task(s) have closed their sockets and exited */
    for (int waited = 0; waited < SERVER_STOP_TIMEOUT_MS; waited += 50) {
        if (s_active_tasks == 0) {
            ESP_LOGI(TAG, "TCP server stopped");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGE(TAG, "TCP server did not stop within %d ms", SERVER_STOP_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}
