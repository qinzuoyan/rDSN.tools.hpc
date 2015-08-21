/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
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

# ifdef __linux__
//# ifdef _WIN32

# include "hpc_network_provider.h"
# include <MSWSock.h>
# include "mix_all_io_looper.h"

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "network.provider.hpc"


namespace dsn
{
    namespace tools
    {
        static int create_tcp_socket(sockaddr_in* addr)
        {
            int s = -1;
            if (s = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0) == -1)
            {
                dwarn("WSASocket failed, err = %s", strerror(errno));
                return -1;
            }

            int nodelay = 1;
            if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(int)) != 0)
            {
                dwarn("setsockopt TCP_NODELAY failed, err = %s", strerror(errno));
            }

            int isopt = 1;
            if (setsockopt(s, SOL_SOCKET, SO_DONTLINGER, (char*)&isopt, sizeof(int)) != 0)
            {
                dwarn("setsockopt SO_DONTLINGER failed, err = %s", strerror(errno));
            }

            //streaming data using overlapped I/O should set the send buffer to zero
            int buflen = 0;
            if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&buflen, sizeof(buflen)) != 0)
            {
                dwarn("setsockopt SO_SNDBUF failed, err = %s", strerror(errno));
            }

            buflen = 8 * 1024 * 1024;
            if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&buflen, sizeof(buflen)) != 0)
            {
                dwarn("setsockopt SO_RCVBUF failed, err = %s", strerror(errno));
            }

            int keepalive = 0;
            if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive, sizeof(keepalive)) != 0)
            {
                dwarn("setsockopt SO_KEEPALIVE failed, err = %s", strerror(errno));
            }

            if (addr != 0)
            {
                if (bind(s, (struct sockaddr*)addr, sizeof(*addr)) != 0)
                {
                    derror("bind failed, err = %s", strerror(errno));
                    closesocket(s);
                    return -1;
                }
            }

            return s;
        }


        hpc_network_provider::hpc_network_provider(rpc_engine* srv, network* inner_provider)
            : connection_oriented_network(srv, inner_provider), _callback(this)
        {
            _listen_fd = -1;
            _looper = get_io_looper(node());
        }

        error_code hpc_network_provider::start(rpc_channel channel, int port, bool client_only)
        {
            if (_listen_fd != -1)
                return ERR_SERVICE_ALREADY_RUNNING;
            
            dassert(channel == RPC_CHANNEL_TCP || channel == RPC_CHANNEL_UDP, 
                "invalid given channel %s", channel.to_string());

            gethostname(_address.name, sizeof(_address.name));
            dsn_address_build(&_address, _address.name, port);

            if (!client_only)
            {
                struct sockaddr_in addr;
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = INADDR_ANY;
                addr.sin_port = htons(port);

                _listen_fd = create_tcp_socket(&addr);
                if (_listen_fd == -1)
                {
                    dassert(false, "");
                }

                int forcereuse = 1;
                if (setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, 
                    (char*)&forcereuse, sizeof(forcereuse)) != 0)
                {
                    dwarn("setsockopt SO_REUSEDADDR failed, err = %d");
                }

                if (listen(_listen_fd, SOMAXCONN) != 0)
                {
                    dwarn("listen failed, err = %s", strerror(errno));
                    return ERR_NETWORK_START_FAILED;
                }
                
                get_looper()->bind_io_handle((dsn_handle_t)_listen_fd, &_callback);
            }
            
            return ERR_OK;
        }

        rpc_client_session_ptr hpc_network_provider::create_client_session(const dsn_address_t& server_addr)
        {
            auto matcher = new_client_matcher();
            auto parser = new_message_parser();

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = 0;

            auto sock = create_tcp_socket(&addr);

            get_looper()->bind_io_handle((dsn_handle_t)sock, &_callback);

            return new hpc_rpc_client_session(sock, parser, *this, server_addr, matcher);
        }

        void hpc_network_provider::on_events_ready(uint32_t events)
        {
            struct sockaddr_in local_addr;
            int len = (int)sizeof(local_addr);
            int s = accept(_listen_fd, (struct sockaddr*)&local_addr, &len);

            if (s == -1)
            {
                derror("accept failed, err = %s", strerror(errno));
            }
            else
            {
                struct sockaddr_in addr;
                memset((void*)&addr, 0, sizeof(addr));

                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = INADDR_ANY;
                addr.sin_port = 0;

                int addr_len = sizeof(addr);
                if (getpeername(s, (struct sockaddr*)&addr, &addr_len)
                    == SOCKET_ERROR)
                {
                    dassert(false, "getpeername failed, err = %d", ::WSAGetLastError());
                }

                dsn_address_t client_addr;
                dsn_address_build_ipv4(&client_addr, ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port));

                auto parser = new_message_parser();
                rpc_server_session_ptr s = new hpc_rpc_server_session(_accept_event.s, parser, *this, client_addr);
                this->on_server_session_accepted(s);
            }
        }

        hpc_rpc_session::hpc_rpc_session(
            int sock,
            std::shared_ptr<dsn::message_parser>& parser
            )
            : _rw_fd(sock), _parser(parser)
        {

        }

        void hpc_rpc_session::do_read(int sz)
        {
            add_reference();

            _read_event.callback = [this](int err, uint32_t length)
            {
                if (err != ERROR_SUCCESS)
                {
                    dwarn("WSARecv failed, err = %d", err);
                    on_failure();
                }
                else
                {
                    int read_next;
                    message_ex* msg = _parser->get_message_on_receive((int)length, read_next);

                    while (msg != nullptr)
                    {
                        this->on_read_completed(msg);
                        msg = _parser->get_message_on_receive(0, read_next);
                    }

                    do_read(read_next);
                }

                release_reference();
            };
            memset(&_read_event.olp, 0, sizeof(_read_event.olp));

            WSABUF buf[1];

            void* ptr = _parser->read_buffer_ptr((int)sz);
            int remaining = _parser->read_buffer_capacity();
            buf[0].buf = (char*)ptr;
            buf[0].len = remaining;

            DWORD bytes = 0;
            DWORD flag = 0;
            int rt = WSARecv(
                _rw_fd,
                buf,
                1,
                &bytes,
                &flag,
                &_read_event.olp,
                NULL
                );

            if (SOCKET_ERROR == rt && (WSAGetLastError() != ERROR_IO_PENDING))
            {
                dwarn("WSARecv failed, err = %d", ::WSAGetLastError());
                release_reference();
                on_failure();
            }
        }

        void hpc_rpc_session::do_write(message_ex* msg)
        {
            add_reference();
            _write_event.callback = [this, msg](int err, uint32_t length)
            {
                if (err != ERROR_SUCCESS)
                {
                    dwarn("WSASend failed, err = %d", err);
                    on_failure();
                }
                else
                {
                    on_write_completed(msg);
                }

                release_reference();
            };
            memset(&_write_event.olp, 0, sizeof(_write_event.olp));

            // make sure header is already in the buffer
            std::vector<dsn_message_parser::send_buf> buffers;
            _parser->prepare_buffers_on_send(msg, buffers);

            static_assert (sizeof(dsn_message_parser::send_buf) == sizeof(WSABUF), "make sure they are compatible");

            DWORD bytes = 0;
            int rt = WSASend(
                _rw_fd,
                (LPWSABUF)&buffers[0],
                (DWORD)buffers.size(),
                &bytes,
                0,
                &_write_event.olp,
                NULL
                );

            if (SOCKET_ERROR == rt && (WSAGetLastError() != ERROR_IO_PENDING))
            {
                dwarn("WSASend failed, err = %d", ::WSAGetLastError());
                release_reference();
                on_failure();
            }
        }

        void hpc_rpc_session::close()
        {
            closesocket(_rw_fd);
            on_closed();
        }

        hpc_rpc_client_session::hpc_rpc_client_session(
            int sock,
            std::shared_ptr<dsn::message_parser>& parser,
            connection_oriented_network& net,
            const dsn_address_t& remote_addr,
            rpc_client_matcher_ptr& matcher
            )
            : rpc_client_session(net, remote_addr, matcher), hpc_rpc_session(sock, parser), _socket(sock)
        {
            _reconnect_count = 0;
            _state = SS_CLOSED;
        }

        void hpc_rpc_client_session::on_failure()
        {
            _state = SS_CLOSED;

            if (++_reconnect_count > 3)
            {
                close();
                on_disconnected();
                return;
            }

            connect();
        }

        void hpc_rpc_client_session::connect()
        {
            session_state closed_state = SS_CLOSED;

            if (!_state.compare_exchange_strong(closed_state, SS_CONNECTING))
                return;
                        
            auto evt = new hpc_network_provider::completion_event;
            evt->callback = [this, evt](int err, uint32_t io_size)
            {
                if (err != ERROR_SUCCESS)
                {
                    dwarn("ConnectEx failed, err = %d", err);
                    this->on_failure();
                }
                else
                {
                    _reconnect_count = 0;
                    _state = SS_CONNECTED;

                    dinfo("client session %s:%hu connected",
                        _remote_addr.name,
                        _remote_addr.port
                        );

                    send_messages();                    
                    do_read();
                }
                this->release_ref(); // added before ConnectEx
                delete evt;
            };

            memset(&evt->olp, 0, sizeof(evt->olp));

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(_remote_addr.ip);
            addr.sin_port = htons(_remote_addr.port);

            this->add_ref(); // released in evt->callback
            BOOL rt = s_lpfnConnectEx(
                _socket,
                (struct sockaddr*)&addr,
                (int)sizeof(addr),
                0,
                0,
                0,
                &evt->olp
                );

            if (!rt && (WSAGetLastError() != ERROR_IO_PENDING))
            {
                dwarn("ConnectEx failed, err = %d", ::WSAGetLastError());
                this->release_ref();
                delete evt;

                on_failure();
            }
        }

        hpc_rpc_server_session::hpc_rpc_server_session(
            int sock,
            std::shared_ptr<dsn::message_parser>& parser,
            connection_oriented_network& net,
            const dsn_address_t& remote_addr
            )
            : rpc_server_session(net, remote_addr), hpc_rpc_session(sock, parser)
        {
            do_read();
        }

    }
}

# endif