#include "trace.h"
#include "client_manager.h"
#include "jpeg2000/file_manager.h"
#include "jpip/jpip.h"
#include "jpip/request.h"
#include "jpip/databin_server.h"
#include "http/response.h"
#include "net/socket_stream.h"

#include "z/zfilter.h"

static const char *CORS = "*";
static const char *NOCACHE = "no-cache";
static const char *STS = "max-age=31536000; includeSubDomains;";

using namespace std;
using namespace net;
using namespace http;
using namespace jpip;
using namespace jpeg2000;

static void send_chunk(SocketStream &strm, const void *buf, size_t len) {
    if (len > 0) {
        strm << hex << len << dec << http::Protocol::CRLF;
        if (strm.Send(buf, len) != (ssize_t) len)
            { /* ERROR("Could not send"); */ }
        strm << http::Protocol::CRLF;
    }
}

void ClientManager::Run(ClientInfo *client_info) {
    bool com_error;
    string req_line;
    jpip::Request req;
    bool pclose = false;
    bool is_opened = false;
    bool send_data = false;
    DataBinServer data_server;

    FileManager file_manager;
    if (!file_manager.Init(cfg.images_folder())) {
        ERROR("The file manager can not be initialized");
        return;
    }

    stringstream head_data, head_data_gzip;
    head_data << http::Header::AccessControlAllowOrigin(CORS)
              << http::Header::StrictTransportSecurity(STS)
              << http::Header::CacheControl(NOCACHE)
              << http::Header::TransferEncoding("chunked")
              << http::Header::ContentType("image/jpp-stream");
    head_data_gzip << head_data.str() << http::Header::ContentEncoding("gzip");

    SocketStream sock_stream(client_info->sock(), 512, 64 * 1024);
    string channel = to_string(client_info->base_id());

    int chunk_len = 0;
    size_t buf_len = cfg.max_chunk_size();
    char *buf = new char[buf_len];

    while (!pclose) {
        bool accept_gzip = false;
        bool send_gzip = false;

        if (cfg.log_requests())
            LOGC(_BLUE, "Waiting for a request ...");

        if (cfg.com_time_out() > 0) {
            if (sock_stream->WaitForInput(cfg.com_time_out() * 1000) == 0) {
                LOG("Communication time-out");
                break;
            }
        }

        com_error = true;
        if (getline(sock_stream, req_line).good())
            com_error = !req.Parse(req_line);

        if (com_error) {
            if (sock_stream->IsValid())
                LOG("Incorrect request received");
            else
                LOG("Connection closed by the client");
            break;
        } else {
            if (cfg.log_requests())
                LOGC(_BLUE, "Request: " << req_line);

            http::Header header;
            int content_length = 0;

            while ((sock_stream >> header).good()) {
                if (header == http::Header::ContentLength())
                    content_length = atoi(header.value.c_str());
                else if (header == http::Header::AcceptEncoding() &&
                         header.value.find("gzip") != string::npos)
                    accept_gzip = true;
            }

            if (req.type == http::Request::POST) {
                stringstream body;
                sock_stream.clear();

                while (content_length--)
                    body.put((char) sock_stream.get());

                req.ParseParameters(body);
            }
            sock_stream.clear();
        }

        const char *err_msg = "";
        pclose = true;
        send_data = false;

        if (req.mask.items.metareq && accept_gzip)
            send_gzip = true;

        if (req.mask.items.cclose) {
            if (!is_opened) {
                err_msg = "Close request received but there is not any channel opened";
                LOG(err_msg);
                /* Only one channel per client supported */
            } else if (req.parameters["cclose"] != "*" && req.parameters["cclose"] != channel) {
                err_msg = "Close request received related to another channel";
                LOG(err_msg);
            } else {
                pclose = false;
                is_opened = false;
                req.cache_model.Clear();
                LOG("The channel " << channel << " has been closed");
                sock_stream << http::Response(200)
                            << http::Header::AccessControlAllowOrigin(CORS)
                            << http::Header::StrictTransportSecurity(STS)
                            << http::Header::CacheControl(NOCACHE)
                            << http::Header::ContentLength("0")
                            << http::Protocol::CRLF << flush;
            }
        } else if (req.mask.items.cnew) {
            if (is_opened) {
                err_msg = "There already is a channel opened. Only one channel per client is supported";
                LOG(err_msg);
            } else {
                string file_name = (req.mask.items.target ? req.parameters["target"] : req.object);

                if (!file_manager.OpenImage(file_name))
                    ERROR("The image file '" << file_name << "' can not be read");
                else {
                    is_opened = true;
                    data_server.Reset();
                    if (!data_server.SetRequest(file_manager, req))
                        ERROR("The server can not process the request");
                    else {
                        LOG("The channel " << channel << " has been opened for the image '" << file_name << "'");

                        sock_stream << http::Response(200)
                                    << http::Header("JPIP-cnew", "cid=" + channel + ",path=jpip,transport=http")
                                    << http::Header("JPIP-tid", file_name)
                                    << http::Header::AccessControlExposeHeaders("JPIP-cnew,JPIP-tid")
                                    << (send_gzip ? head_data_gzip.str() : head_data.str())
                                    << http::Protocol::CRLF << flush;
                        send_data = true;
                    }
                }
            }
        } else if (req.mask.items.cid) {
            if (!is_opened) {
                err_msg = "Request received but no channel is opened";
                LOG(err_msg);
            } else {
                if (req.parameters["cid"] != channel) {
                    err_msg = "Request related to another channel";
                    LOG(err_msg);
                } else if (!data_server.SetRequest(file_manager, req))
                    ERROR("The server can not process the request");
                else {
                    sock_stream << http::Response(200)
                                << (send_gzip ? head_data_gzip.str() : head_data.str())
                                << http::Protocol::CRLF << flush;
                    send_data = true;
                }
            }
        } else {
            err_msg = "Invalid request (channel parameter not found)";
            LOG(err_msg);
        }

        pclose = pclose && !send_data;

        if (pclose) {
            size_t err_msg_len = strlen(err_msg);
            sock_stream << http::Response(500)
                        << http::Header::AccessControlAllowOrigin(CORS)
                        << http::Header::StrictTransportSecurity(STS)
                        << http::Header::CacheControl(NOCACHE)
                        << http::Header::ContentLength(to_string(err_msg_len))
                        << http::Protocol::CRLF << flush;
            if (err_msg_len) {
                if (sock_stream->Send(err_msg, err_msg_len) != (ssize_t) err_msg_len)
                    { /* ERROR("Could not send"); */ }
            }
        } else if (send_data) {
            if (!send_gzip)
                for (bool last = false; !last;) {
                    chunk_len = buf_len;

                    if (!data_server.GenerateChunk(file_manager, buf, &chunk_len, &last)) {
                        ERROR("A new data chunk could not be generated");
                        pclose = true;
                        break;
                    }
                    send_chunk(sock_stream, buf, chunk_len);
                }
            else {
                void *obj = zfilter_new();

                for (bool last = false; !last;) {
                    chunk_len = buf_len;

                    if (!data_server.GenerateChunk(file_manager, buf, &chunk_len, &last)) {
                        ERROR("A new data chunk could not be generated");
                        pclose = true;
                        break;
                    }

                    if (chunk_len > 0)
                        zfilter_write(obj, buf, chunk_len);
                }

                size_t nbytes;
                const uint8_t *out = (uint8_t *) zfilter_bytes(obj, &nbytes);

                while (nbytes > buf_len) {
                    send_chunk(sock_stream, out, buf_len);
                    nbytes -= buf_len;
                    out += buf_len;
                }
                if (nbytes > 0)
                    send_chunk(sock_stream, out, nbytes);

                zfilter_del(obj);
            }

            sock_stream << "0" << http::Protocol::CRLF << http::Protocol::CRLF << flush;
        }
    }

    delete[] buf;

    sock_stream->Close();
    close(client_info->sock());
}

void ClientManager::RunBasic(ClientInfo *client_info) {
    jpip::Request req;
    size_t buf_len = 5000;
    char *buf = new char[buf_len];
    SocketStream sock_stream(client_info->sock());

    for (;;) {
        LOG("Waiting for a request ...");

        if (cfg.com_time_out() > 0) {
            if (sock_stream->WaitForInput(cfg.com_time_out() * 1000) == 0) {
                LOG("Communication time-out");
                sock_stream->Close();
                break;
            }
        }

        if (!(sock_stream >> req).good()) {
            if (sock_stream->IsValid())
                LOG("Incorrect request received");
            else
                LOG("Connection closed by the client");
            sock_stream->Close();
            break;
        } else {
            http::Header header;
            while ((sock_stream >> header).good());
            sock_stream.clear();
        }

        sock_stream << http::Response(200)
                    << http::Header("JPIP-cnew", "cid=C0,path=jpip,transport=http")
                    << http::Header("JPIP-tid", "T0")
                    << http::Header::AccessControlAllowOrigin(CORS)
                    << http::Header::AccessControlExposeHeaders("JPIP-cnew,JPIP-tid")
                    << http::Header::StrictTransportSecurity(STS)
                    << http::Header::CacheControl(NOCACHE)
                    << http::Header::ContentLength(to_string(buf_len))
                    << http::Header::ContentType("image/jpp-stream")
                    << http::Protocol::CRLF;
        if (sock_stream.Send(buf, buf_len) != (ssize_t) buf_len)
            ERROR("Could not send");
        sock_stream << flush;
    }

    delete[] buf;
}
