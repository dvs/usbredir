/* usbredirserver.c simple usb network redirection tcp/ip server (host).

   Copyright 2010-2011 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include "usbredirhost.h"

#define SERVER_VERSION "usbredirserver " PACKAGE_VERSION

static int verbose = usbredirparser_info;
static int client_fd = -1, running = 1;
static int wait_mode = 0;
static int wait_timeout = 3;
static libusb_context *ctx;
static struct usbredirhost *host;

static int usbvendor  = -1;
static int usbproduct = -1;
static int usbbus     = -1;
static int usbaddr    = -1;
static libusb_device_handle *handle = NULL;

static const struct option longopts[] = {
    { "port", required_argument, NULL, 'p' },
    { "verbose", required_argument, NULL, 'v' },
    { "wait", no_argument, NULL, 'w' },
    { "wait-timeout", required_argument, NULL, 't' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 }
};

static void usbredirserver_log(void *priv, int level, const char *msg)
{
    if (level <= verbose)
        fprintf(stderr, "%s\n", msg);
}

static void
#if defined __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
va_log(int level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    n = sprintf(buf, "usbredirserver: ");
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    usbredirserver_log(NULL, level, buf);
}

#ifdef ERROR /* defined on WIN32 */
#undef ERROR
#endif
#define ERROR(...)   va_log(usbredirparser_error, __VA_ARGS__)
#define WARNING(...) va_log(usbredirparser_warning, __VA_ARGS__)
#define INFO(...)    va_log(usbredirparser_info, __VA_ARGS__)
#define DEBUG(...)   va_log(usbredirparser_debug, __VA_ARGS__)

static int usbredirserver_read(void *priv, uint8_t *data, int count)
{
    int r;
    errno = 0;
    r = read(client_fd, data, count);
    DEBUG("usbredirserver_read : client_fd = %d, read bytes = %d/%d, errno = %d", client_fd, r, count, errno);
    if (r < 0) {
        if (errno == EAGAIN)
            return 0;
        return -1;
    }
    if (r == 0) { /* Client disconnected */
        close(client_fd);
        client_fd = -1;
    }
    return r;
}

static int usbredirserver_write(void *priv, uint8_t *data, int count)
{
    int r;
    errno = 0;
    r = write(client_fd, data, count);
    DEBUG("usbredirserver_write : client_fd = %d, write bytes = %d/%d, errno = %d", client_fd, r, count, errno);
    if (r < 0) {
        if (errno == EAGAIN)
            return 0;
        if (errno == EPIPE) { /* Client disconnected */
            close(client_fd);
            client_fd = -1;
            return 0;
        }
        return -1;
    }
    return r;
}

static void usage(int exit_code, char *argv0)
{
    fprintf(exit_code? stderr:stdout,
        "Usage: %s [-p|--port <port>] [-v|--verbose <0-5>] [-w|--wait] [-t|--wait-timeout #] <usbbus-usbaddr|vendorid:prodid>\n",
        argv0);
    exit(exit_code);
}

static void invalid_usb_device_id(char *usb_device_id, char *argv0)
{
    fprintf(stderr, "Invalid usb device identifier: %s\n", usb_device_id);
    usage(1, argv0);
}

static void find_device()
{
    /* Try to find the specified usb device */
    if (usbvendor != -1) {
        handle = libusb_open_device_with_vid_pid(ctx, usbvendor,
                                                    usbproduct);
        if (!handle) {
            INFO("Could not open an usb-device with vid:pid %04x:%04x", usbvendor, usbproduct);
        }
    } else {
        libusb_device **list = NULL;
        ssize_t i, n;

        n = libusb_get_device_list(ctx, &list);
        for (i = 0; i < n; i++) {
            if (libusb_get_bus_number(list[i]) == usbbus &&
                    libusb_get_device_address(list[i]) == usbaddr)
                break;
        }
        if (i < n) {
            if (libusb_open(list[i], &handle) != 0) {
                INFO("Could not open usb-device at bus-addr %d-%d", usbbus, usbaddr);
            }
        } else {
            INFO("Could not find an usb-device at bus-addr %d-%d\n", usbbus, usbaddr);
        }
        libusb_free_device_list(list, 1);
    }
}

static void run_main_loop(void)
{
    const struct libusb_pollfd **pollfds = NULL;
    fd_set readfds, writefds;
    int i, n, nfds;
    struct timeval timeout, *timeout_p;

    INFO("Starting run_main_loop...");
    int wait_mode_state = 0;
    while (running && client_fd != -1) {
        DEBUG("Looping in run_main_loop...");
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        FD_SET(client_fd, &readfds);
        if (usbredirhost_has_data_to_write(host)) {
            FD_SET(client_fd, &writefds);
        }
        nfds = client_fd + 1;

        free(pollfds);
        pollfds = libusb_get_pollfds(ctx);
        for (i = 0; pollfds && pollfds[i]; i++) {
            if (pollfds[i]->events & POLLIN) {
                FD_SET(pollfds[i]->fd, &readfds);
            }
            if (pollfds[i]->events & POLLOUT) {
                FD_SET(pollfds[i]->fd, &writefds);
            }
            if (pollfds[i]->fd >= nfds)
                nfds = pollfds[i]->fd + 1;
        }

        struct timeval default_tv = {wait_timeout, 0};
        if (libusb_get_next_timeout(ctx, &timeout) == 1) {
            timeout_p = &timeout;
        } else {
            if (wait_mode) {
                timeout_p = &default_tv;
            } else {
                timeout_p = NULL;
            }
        }
        n = select(nfds, &readfds, &writefds, NULL, timeout_p);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }
        memset(&timeout, 0, sizeof(timeout));
        if (n == 0) {
            libusb_handle_events_timeout(ctx, &timeout);
            if (!wait_mode) {
                continue;
            }
        }

        if (FD_ISSET(client_fd, &readfds)) {
            DEBUG("before usbredirhost_read_guest_data(host=0%lx)", (unsigned long) host);
            int error = usbredirhost_read_guest_data(host);
            if (error) {
                DEBUG("usbredirhost_read_guest_data: error = %d", error);
                break;
            }
        }
        /* usbredirhost_read_guest_data may have detected client disconnect */
        if (client_fd == -1) {
            break;
        }

        if (FD_ISSET(client_fd, &writefds)) {
            int error = usbredirhost_write_guest_data(host);
            if (error) {
                DEBUG("usbredirhost_write_guest_data: error = %d", error);
                break;
            }
        }

        for (i = 0; pollfds && pollfds[i]; i++) {
            if (FD_ISSET(pollfds[i]->fd, &readfds) ||
                FD_ISSET(pollfds[i]->fd, &writefds)) {
                libusb_handle_events_timeout(ctx, &timeout);
                break;
            }
        }
        if (wait_mode) {
            int disconnected = usbredirhost_is_disconnected(host);
            DEBUG("disconnected = %d, wait_mode_state = %d", disconnected, wait_mode_state);
            if (disconnected == 0 && wait_mode_state == 0) {
                wait_mode_state = 1; /* the device is connected now */
                continue;
            }
            if (wait_mode_state == 1 && client_fd != -1) {
                if (n == 0) {
                    /* timeout in select, check libusb for presence of the device */
                    int config;
                    int error = libusb_get_configuration(handle, &config);
                    DEBUG("libusb_get_configuration: error = %d", error);
                    if (error && disconnected == 0) {
                        usbredirhost_disconnect(host);
                        disconnected = 1;
                    }
                }
                if (disconnected == 1) {
                    usbredirhost_write_guest_data(host);
                    sleep(wait_timeout);
                    usbredirhost_read_guest_data(host);
                    while(client_fd != -1) {
                        libusb_exit(ctx);
                        if (libusb_init(&ctx)) {
                            fprintf(stderr, "Could not init libusb\n");
                            exit(1);
                        }
                        libusb_set_debug(ctx, verbose);

                        find_device();

                        if (!handle) {
                            if (usbvendor != -1) {
                                INFO("Waiting for vid:pid %04x:%04x ...", usbvendor, usbproduct);
                            } else {
                                INFO("Waiting for usb-device at bus-addr %d-%d ...", usbbus, usbaddr);
                            }
                            sleep(wait_timeout);
                        } else {
                            INFO("opened handle = 0x%lx", (long unsigned) handle);
                            uint32_t peer_caps[USB_REDIR_CAPS_SIZE];
                            usbredirhost_save_caps(host, peer_caps);
                            host = usbredirhost_open(ctx, handle, usbredirserver_log,
                                                    usbredirserver_read, usbredirserver_write,
                                                    NULL, SERVER_VERSION, verbose, usbredirparser_fl_no_hello);
                            usbredirhost_restore_caps_and_send_device_connect(host, peer_caps);
                            usbredirhost_write_guest_data(host);
                            break;
                        }
                    }
                    wait_mode_state = 0;
                }
            }
        }
    }
    INFO("Leaving run_main_loop, client_fd = %d.", client_fd);
    if (client_fd != -1) { /* Broken out of the loop because of an error ? */
        close(client_fd);
        client_fd = -1;
    }
    free(pollfds);
}

static void quit_handler(int sig)
{
    running = 0;
}

int main(int argc, char *argv[])
{
    int o, flags, server_fd = -1;
    char *endptr, *delim;
    int port = 4000;
    int on = 1;
    struct sockaddr_in6 serveraddr;
    struct sigaction act;

    while ((o = getopt_long(argc, argv, "hwp:v:t:", longopts, NULL)) != -1) {
        switch (o) {
        case 'p':
            port = strtol(optarg, &endptr, 10);
            if (*endptr != '\0') {
                fprintf(stderr, "Invalid value for --port: '%s'\n", optarg);
                usage(1, argv[0]);
            }
            break;
        case 'v':
            verbose = strtol(optarg, &endptr, 10);
            if (*endptr != '\0') {
                fprintf(stderr, "Invalid value for --verbose: '%s'\n", optarg);
                usage(1, argv[0]);
            }
            break;
        case 'w':
            wait_mode = 1;
            break;
        case 't':
            wait_timeout = strtol(optarg, &endptr, 10);
            if (*endptr != '\0') {
                fprintf(stderr, "Inalid value for --wait-timeout: '%s'\n", optarg);
                usage(1, argv[0]);
            }
            break;
        case '?':
        case 'h':
            usage(o == '?', argv[0]);
            break;
        }
    }
    if (optind == argc) {
        fprintf(stderr, "Missing usb device identifier argument\n");
        usage(1, argv[0]);
    }
    delim = strchr(argv[optind], '-');
    if (delim && delim[1]) {
        usbbus = strtol(argv[optind], &endptr, 10);
        if (*endptr != '-') {
            invalid_usb_device_id(argv[optind], argv[0]);
        }
        usbaddr = strtol(delim + 1, &endptr, 10);
        if (*endptr != '\0') {
            invalid_usb_device_id(argv[optind], argv[0]);
        }
    } else {
        delim = strchr(argv[optind], ':');
        if (!delim || !delim[1]) {
            invalid_usb_device_id(argv[optind], argv[0]);
        }
        usbvendor = strtol(argv[optind], &endptr, 16);
        if (*endptr != ':') {
            invalid_usb_device_id(argv[optind], argv[0]);
        }
        usbproduct = strtol(delim + 1, &endptr, 16);
        if (*endptr != '\0') {
            invalid_usb_device_id(argv[optind], argv[0]);
        }
    }
    optind++;
    if (optind != argc) {
        fprintf(stderr, "Excess non option arguments\n");
        usage(1, argv[0]);
    }

    memset(&act, 0, sizeof(act));
    act.sa_handler = quit_handler;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

    if (libusb_init(&ctx)) {
        fprintf(stderr, "Could not init libusb\n");
        exit(1);
    }

    libusb_set_debug(ctx, verbose);

    server_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Error creating ipv6 socket");
        exit(1);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) {
        perror("Error setsockopt(SO_REUSEADDR) failed");
        exit(1);
    }

    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin6_family = AF_INET6;
    serveraddr.sin6_port   = htons(port);
    serveraddr.sin6_addr   = in6addr_any;

    if (bind(server_fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr))) {
        fprintf(stderr, "Error binding port %d: %s\n", port, strerror(errno));
        exit(1);
    }

    if (listen(server_fd, 1)) {
        perror("Error listening");
        exit(1);
    }

    while (running) {
        INFO("Looping in main (client_fd = %d, handle = 0x%lx)...", client_fd, (unsigned long) handle);
        if (client_fd == -1 || !wait_mode) {
            client_fd = accept(server_fd, NULL, 0);
            if (client_fd == -1) {
                if (errno == EINTR) {
                    continue;
                }
                perror("accept");
                break;
            }
            flags = fcntl(client_fd, F_GETFL);
            if (flags == -1) {
                perror("fcntl F_GETFL");
                break;
            }
            flags = fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            if (flags == -1) {
                perror("fcntl F_SETFL O_NONBLOCK");
                break;
            }
        } else {
            sleep(wait_timeout);
        }

        find_device();

        if (!handle) {
            if (!wait_mode) {
                close(client_fd);
            }
            continue;
        }

        host = usbredirhost_open(ctx, handle, usbredirserver_log,
                                 usbredirserver_read, usbredirserver_write,
                                 NULL, SERVER_VERSION, verbose, 0);
        if (!host)
            exit(1);
        run_main_loop();
        usbredirhost_close(host);
        handle = NULL;
    }

    close(server_fd);
    libusb_exit(ctx);
    exit(0);
}
