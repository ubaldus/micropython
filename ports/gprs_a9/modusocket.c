/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Paul Sokolovsky
 * Copyright (c) 2016 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "modcellular.h"
#include "errno.h"

#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/binary.h"
#include "py/objarray.h"
#include "py/objexcept.h"
#include "py/mperrno.h"
#include "py/stream.h"

#include "api_network.h"
#include "api_socket.h"
#include "sdk_init.h"

#include "stdio.h"

#define SOCKET_POLL_US (100000)
#define ERRNO (g_InterfaceVtbl->Socket_GetLastError())

// -------
// Classes
// -------

NORETURN static void exception_from_errno(int _errno) {
    // Here we need to convert from lwip errno values to MicroPython's standard ones
    if (_errno == EINPROGRESS) {
        _errno = MP_EINPROGRESS;
    }
    mp_raise_OSError(_errno);
}

typedef struct _socket_obj_t {
    mp_obj_base_t base;
    int fd;
    uint8_t domain;
    uint8_t type;
    uint8_t proto;
    bool peer_closed;
    unsigned int retries;
} socket_obj_t;

void _socket_settimeout(socket_obj_t *sock, uint64_t timeout_ms) {
    // Rather than waiting for the entire timeout specified, we wait sock->retries times
    // for SOCKET_POLL_US each, checking for a MicroPython interrupt between timeouts.
    // with SOCKET_POLL_MS == 100ms, sock->retries allows for timeouts up to 13 years.
    // if timeout_ms == UINT64_MAX, wait forever.
    sock->retries = (timeout_ms == UINT64_MAX) ? UINT_MAX : timeout_ms * 1000 / SOCKET_POLL_US;

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = timeout_ms ? SOCKET_POLL_US : 0
    };
    setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO, (const void *)&timeout, sizeof(timeout));
    setsockopt(sock->fd, SOL_SOCKET, SO_RCVTIMEO, (const void *)&timeout, sizeof(timeout));
    fcntl(sock->fd, F_SETFL, timeout_ms ? 0 : O_NONBLOCK);
}

static int _socket_getaddrinfo2(const mp_obj_t host, const mp_obj_t port, struct sockaddr_in *resp) {

    const char *host_str = mp_obj_str_get_str(host);
    const int port_int = mp_obj_get_int(port);

    if (host_str[0] == '\0') {
        // a host of "" is equivalent to the default/all-local IP address
        host_str = "0.0.0.0";
    }

    char address[16];
    if (DNS_GetHostByName2((uint8_t*)host_str, (uint8_t*)address) != 0) {
        return -1;
    }

    memset(resp, 0, sizeof(*resp));
    resp->sin_family = AF_INET;
    resp->sin_port = htons(port_int);

    MP_THREAD_GIL_EXIT();
    int res = inet_pton(AF_INET, address, &resp->sin_addr);
    MP_THREAD_GIL_ENTER();

    return res;
}

int _socket_getaddrinfo(const mp_obj_t addrtuple, struct sockaddr_in *resp) {
    mp_uint_t len = 0;
    mp_obj_t *elem;
    mp_obj_get_array(addrtuple, &len, &elem);
    if (len != 2) return -1;
    return _socket_getaddrinfo2(elem[0], elem[1], resp);
}

mp_obj_t socket_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {

    enum { ARG_af, ARG_type, ARG_proto };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_af, MP_ARG_INT, {.u_int = AF_INET} },
        { MP_QSTR_type, MP_ARG_INT, {.u_int = SOCK_STREAM} },
        { MP_QSTR_proto, MP_ARG_INT, {.u_int = IPPROTO_TCP} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    socket_obj_t *self = m_new_obj_with_finaliser(socket_obj_t);
    self->base.type = type;
    self->peer_closed = false;

    switch (args[ARG_af].u_int) {
        case AF_INET:
            self->domain = AF_INET;
            break;
        case AF_INET6:
            self->domain = AF_INET6;
            break;
        default:
            mp_raise_ValueError("Unknown 'af' argument value");
            break;
    }

    switch (args[ARG_type].u_int) {
        case SOCK_STREAM:
            self->type = SOCK_STREAM;
            break;
        case SOCK_DGRAM:
            self->type = SOCK_DGRAM;
            break;
        default:
            mp_raise_ValueError("Unknown 'type' argument");
            break;
    }

    switch (args[ARG_proto].u_int) {
        case IPPROTO_TCP:
            self->proto = IPPROTO_TCP;
            break;
        case IPPROTO_UDP:
            self->proto = IPPROTO_UDP;
            break;
        default:
            mp_raise_ValueError("Unknown protocol");
            return mp_const_none;
    }

    self->fd = socket(self->domain, self->type, self->proto);
    if (self->fd < 0) {
        mp_raise_NotImplementedError("Failed to create the socket but the error is unknown");
    }
    _socket_settimeout(self, UINT64_MAX);

    return MP_OBJ_FROM_PTR(self);
}

STATIC mp_obj_t socket_close(mp_obj_t self_in) {
    // ========================================
    // Closes the socket.
    // ========================================
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);

    int r = close(self->fd);

    if (r < 0) {
        exception_from_errno(ERRNO);
    }

    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_1(socket_close_obj, &socket_close);

STATIC mp_obj_t socket_bind(mp_obj_t self_in, mp_obj_t address) {
    // ========================================
    // Binds the socket.
    // Args:
    //     address (tuple): address to bind to;
    // ========================================
    mp_raise_NotImplementedError("Server capabilities are not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_bind_obj, &socket_bind);

STATIC mp_obj_t socket_listen(size_t n_args, const mp_obj_t *args) {
    // ========================================
    // Sets the socket to listen for incoming connections.
    // Args:
    //     backlog (int): the number of unaccepted connections
    //     that the system will allow before refusing new connections.
    // ========================================
    mp_raise_NotImplementedError("Server capabilities are not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_listen_obj, 1, 2, &socket_listen);

STATIC mp_obj_t socket_accept(mp_obj_t self_in) {
    // ========================================
    // Accepts the connection.
    // ========================================
    mp_raise_NotImplementedError("Server capabilities are not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_1(socket_accept_obj, &socket_accept);

STATIC mp_obj_t socket_connect(mp_obj_t self_in, mp_obj_t ipv4) {
    // ========================================
    // Connects.
    // Args:
    //     ipv4 (tuple): a tuple of (address, port);
    // ========================================
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);

    struct sockaddr_in res;
    _socket_getaddrinfo(ipv4, &res);

    MP_THREAD_GIL_EXIT();
    int r = connect(self->fd, (struct sockaddr*)&res, sizeof(struct sockaddr_in));
    MP_THREAD_GIL_ENTER();

    if (r < 0) {
        exception_from_errno(ERRNO);
    }

    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_connect_obj, &socket_connect);

int _socket_send(socket_obj_t *sock, const char *data, size_t datalen) {
    int sentlen = 0;
    for (int i=0; i<=sock->retries && sentlen < datalen; i++) {

        MP_THREAD_GIL_EXIT();
        int r = write(sock->fd, data + sentlen, datalen - sentlen);
        MP_THREAD_GIL_ENTER();

        if (r < 0 && ERRNO != EWOULDBLOCK) exception_from_errno(ERRNO);
        if (r > 0) sentlen += r;
        // check_for_exceptions();
    }
    if (sentlen == 0) mp_raise_OSError(MP_ETIMEDOUT);
    return sentlen;
}

STATIC mp_obj_t socket_send(mp_obj_t self_in, mp_obj_t bytes) {
    // ========================================
    // Sends bytes.
    // Args:
    //     bytes (str, bytearray): bytes to send;
    // ========================================
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);

    mp_uint_t data_len;
    const char* data = mp_obj_str_get_data(bytes, &data_len);
    int r = _socket_send(self, data, data_len);

    return mp_obj_new_int(r);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_send_obj, &socket_send);

STATIC mp_obj_t socket_sendall(mp_obj_t self_in, mp_obj_t bytes) {
    // ========================================
    // Sends all bytes chunk by chunk.
    // Args:
    //     bytes (str, bytearray): bytes to send;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_sendall_obj, &socket_sendall);

// XXX this can end up waiting a very long time if the content is dribbled in one character
// at a time, as the timeout resets each time a recvfrom succeeds ... this is probably not
// good behaviour.
STATIC mp_uint_t _socket_read_data(mp_obj_t self_in, void *buf, size_t size,
    struct sockaddr *from, socklen_t *from_len, int *errcode) {
    socket_obj_t *sock = MP_OBJ_TO_PTR(self_in);

    // If the peer closed the connection then the lwIP socket API will only return "0" once
    // from lwip_recvfrom_r and then block on subsequent calls.  To emulate POSIX behaviour,
    // which continues to return "0" for each call on a closed socket, we set a flag when
    // the peer closed the socket.
    if (sock->peer_closed) {
        return 0;
    }

    // XXX Would be nicer to use RTC to handle timeouts
    for (int i = 0; i <= sock->retries; ++i) {

        MP_THREAD_GIL_EXIT();
        int r = recvfrom(sock->fd, buf, size, 0, from, from_len);
        MP_THREAD_GIL_ENTER();

        if (r == 0) sock->peer_closed = true;
        if (r >= 0) return r;
        if (ERRNO != EWOULDBLOCK) {
            *errcode = ERRNO;
            return MP_STREAM_ERROR;
        }
        // check_for_exceptions();
    }

    *errcode = sock->retries == 0 ? MP_EWOULDBLOCK : MP_ETIMEDOUT;
    return MP_STREAM_ERROR;
}

mp_obj_t _socket_recvfrom(mp_obj_t self_in, mp_obj_t len_in,
        struct sockaddr *from, socklen_t *from_len) {
    size_t len = mp_obj_get_int(len_in);
    vstr_t vstr;
    vstr_init_len(&vstr, len);

    int errcode;
    mp_uint_t r = _socket_read_data(self_in, vstr.buf, len, from, from_len, &errcode);
    if (r == MP_STREAM_ERROR) {
        exception_from_errno(errcode);
    }

    vstr.len = r;
    return mp_obj_new_str_from_vstr(&mp_type_bytes, &vstr);
}

STATIC mp_obj_t socket_recv(mp_obj_t self_in, mp_obj_t bufsize) {
    // ========================================
    // Receives bytes.
    // Args:
    //     bufsize (int): output array size;
    // ========================================
    return _socket_recvfrom(self_in, bufsize, NULL, NULL);
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_recv_obj, &socket_recv);

STATIC mp_obj_t socket_sendto(mp_obj_t self_in, mp_obj_t bytes, mp_obj_t address) {
    // ========================================
    // Connects and sends bytes.
    // Args:
    //     bytes (str, byterray): bytes to send;
    //     address (tuple): destination address;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_3(socket_sendto_obj, &socket_sendto);

STATIC mp_obj_t socket_recvfrom(mp_obj_t self_in, mp_obj_t bufsize) {
    // ========================================
    // Receives bytes.
    // Args:
    //     bufsize (int): output array size;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_recvfrom_obj, &socket_recvfrom);

STATIC mp_obj_t socket_setsockopt(size_t n_args, const mp_obj_t *args) {
    // ========================================
    // Sets socket options.
    // Args:
    //     level (int): option level;
    //     optname (int): option to set;
    //     value (int): value to set;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_setsockopt_obj, 4, 4, &socket_setsockopt);

STATIC mp_obj_t socket_settimeout(mp_obj_t self_in, mp_obj_t value) {
    // ========================================
    // Sets the timeout for socket operations.
    // Args:
    //     timeout (int): timeout in seconds;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_settimeout_obj, &socket_settimeout);

STATIC mp_obj_t socket_setblocking(mp_obj_t self_in, mp_obj_t flag) {
    // ========================================
    // Sets blocking or non-blocking socket mode.
    // Args:
    //     flag (bool): blocking or non-blocking mode;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_setblocking_obj, &socket_setblocking);

STATIC mp_obj_t socket_makefile(size_t n_args, const mp_obj_t *args) {
    // ========================================
    // Returns a file object associated with the socket.
    // Args:
    //     mode (str): file mode;
    //     buffering (int): buffer size (not supported);
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_makefile_obj, 1, 3, socket_makefile);

STATIC mp_obj_t socket_read(size_t n_args, const mp_obj_t *args) {
    // ========================================
    // Reads data from the socket up to `size` or until EOF.
    // Args:
    //     size (int): the size of the data to read;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_read_obj, 1, 2, socket_read);

STATIC mp_obj_t socket_readinto(size_t n_args, const mp_obj_t *args) {
    // ========================================
    // Reads data from the socket into the buffer up to `size` or until EOF.
    // Args:
    //     buf (bytearray): the buffer to write into;
    //     size (int): the size of the data to read;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_readinto_obj, 2, 3, socket_readinto);

STATIC mp_obj_t socket_readline(mp_obj_t self_in) {
    // ========================================
    // Reads a line of text.
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_1(socket_readline_obj, &socket_readline);

STATIC mp_obj_t socket_write(mp_obj_t self_in, mp_obj_t buf) {
    // ========================================
    // Writes all data into the socket.
    // Args:
    //     buf (bytearray, str): data to write;
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_write_obj, &socket_write);

STATIC const mp_rom_map_elem_t socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&socket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&socket_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&socket_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&socket_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&socket_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&socket_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall), MP_ROM_PTR(&socket_sendall_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&socket_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendto), MP_ROM_PTR(&socket_sendto_obj) },
    { MP_ROM_QSTR(MP_QSTR_recvfrom), MP_ROM_PTR(&socket_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_setsockopt), MP_ROM_PTR(&socket_setsockopt_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&socket_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking), MP_ROM_PTR(&socket_setblocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_makefile), MP_ROM_PTR(&socket_makefile_obj) },

    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&socket_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&socket_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&socket_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&socket_write_obj) },
};

STATIC MP_DEFINE_CONST_DICT(socket_locals_dict, socket_locals_dict_table);

STATIC const mp_obj_type_t socket_type = {
    { &mp_type_type },
    .name = MP_QSTR_socket,
    .make_new = socket_make_new,
    .locals_dict = (mp_obj_dict_t*)&socket_locals_dict,
};

// -------
// Methods
// -------

STATIC mp_obj_t get_local_ip(void) {
    // ========================================
    // Retrieves the local IP address.
    // Returns:
    //     A string with the assigned IP address.
    // ========================================
    char ip[16];
    if (!Network_GetIp(ip, sizeof(ip))) {
        mp_raise_ValueError("Failed to retrieve the local IP address");
        return mp_const_none;
    }
    return mp_obj_new_str(ip, strlen(ip));
}

STATIC MP_DEFINE_CONST_FUN_OBJ_0(get_local_ip_obj, get_local_ip);

STATIC mp_obj_t getaddrinfo(size_t n_args, const mp_obj_t *args) {
    // ========================================
    // Translates host/port into arguments to socket constructor.
    // Args:
    //     host (str): the host name;
    //     port (int): port number;
    //     af (int): address family: AF_INET or AF_INET6;
    //     type: (int): future socket type: SOCK_STREAM or SOCK_DGRAM;
    //     proto (int): future protocol: IPPROTO_TCP or IPPROTO_UDP;
    //     flag (int): additional socket flags;
    // Returns:
    //     A 5-tuple with arguments to `usocket.socket`.
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(getaddrinfo_obj, 2, 6, getaddrinfo);

STATIC mp_obj_t modusocket_inet_ntop(mp_obj_t af, mp_obj_t bin_addr) {
    // ========================================
    // Converts a binary address into textual representation.
    // Args:
    //     af (int): address family: AF_INET or AF_INET6;
    //     bin_addr (bytearray): binary address;
    // Returns:
    //     A string with the address.
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(modusocket_inet_ntop_obj, modusocket_inet_ntop);

STATIC mp_obj_t modusocket_inet_pton(mp_obj_t af, mp_obj_t txt_addr) {
    // ========================================
    // Converts a text address into binary representation.
    // Args:
    //     af (int): address family: AF_INET or AF_INET6;
    //     txt_addr (str): address as text;
    // Returns:
    //     A bytearray address.
    // ========================================
    mp_raise_NotImplementedError("Not implemented yet");
    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(modusocket_inet_pton_obj, modusocket_inet_pton);

STATIC const mp_map_elem_t mp_module_usocket_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_usocket) },

    { MP_OBJ_NEW_QSTR(MP_QSTR_socket), (mp_obj_t)MP_ROM_PTR(&socket_type) },

    { MP_OBJ_NEW_QSTR(MP_QSTR_get_local_ip), (mp_obj_t)&get_local_ip_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_getaddrinfo), (mp_obj_t)&getaddrinfo_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_inet_ntop), (mp_obj_t)&modusocket_inet_ntop },
    { MP_OBJ_NEW_QSTR(MP_QSTR_inet_pton), (mp_obj_t)&modusocket_inet_pton },
};

STATIC MP_DEFINE_CONST_DICT(mp_module_usocket_globals, mp_module_usocket_globals_table);

const mp_obj_module_t usocket_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_usocket_globals,
};