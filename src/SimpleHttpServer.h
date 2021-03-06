//
// Created by 孙宇 on 2020/7/30.
//
/***********************************************************************************************
 ***                                Y S H E N - S T U D I O S                                ***
 ***********************************************************************************************
                                                                                              
                  Project Name : CppLearningDemos 
                                                                                              
                     File Name : SimpleHttpServer.h 
                                                                                              
                    Programmer : 孙宇 
                                                                                              
                    Start Date : 2020/7/30 
                                                                                              
                   Last Update : 2020/7/30 
                                                                                              
 *---------------------------------------------------------------------------------------------*
  Description:           
        简易异步Http服务端示例
  
 *---------------------------------------------------------------------------------------------*
  Functions:
 *---------------------------------------------------------------------------------------------*
  Updates:
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifndef CPPLEARNINGDEMOS_SIMPLEHTTPSERVER_H
#define CPPLEARNINGDEMOS_SIMPLEHTTPSERVER_H

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <utility>
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/config.hpp>
#include "RequestHandler.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
/**
 * shared_from_this() 的意义
 * 需求: 在类的内部需要自身的shared_ptr 而不是this裸指针
 * 场景: 在类中发起一个异步操作, callback回来要保证发起操作的对象仍然有效.
 * 相应的，类也需要enable_shared_from_this
 */
class SimpleHttpServer : public std::enable_shared_from_this<SimpleHttpServer> {
private:
    struct send_lambda {
        SimpleHttpServer &self_;

        explicit send_lambda(SimpleHttpServer &self)
                : self_(self) {}

        template<bool isRequest, class Body, class Fields>
        void operator()(http::message<isRequest, Body, Fields> &&msg) const {
            //msg生命周期必须持续在异步操作中，所以用一个shared_ptr来管理
            auto sp = std::make_shared<http::message<isRequest, Body, Fields>>(std::move(msg));
            self_.res_ = sp;
            http::async_write(
                    self_.stream_,
                    *sp,
                    beast::bind_front_handler(
                            &SimpleHttpServer::on_write,
                            self_.shared_from_this(),
                            sp->need_eof()));
        }
    };

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    std::shared_ptr<std::string const> doc_root_;
    http::request<http::string_body> req_;
    std::shared_ptr<void> res_; //用void ，emmm？
    send_lambda lambda_;

public:
    SimpleHttpServer(
            tcp::socket &&socket,
            std::shared_ptr<std::string const> doc_root
    ) : stream_(std::move(socket)),
        doc_root_(std::move(doc_root)),
        lambda_(*this) {}

    void run() {
        //用strand保证线程安全
        net::dispatch(stream_.get_executor(),
                      beast::bind_front_handler(
                              &SimpleHttpServer::do_read,
                              shared_from_this()));
    }

    void do_read() {
        req_ = {};//每次读取前清空请求载体
        //设置超时并开始读取请求
        stream_.expires_after(std::chrono::seconds(30));
        http::async_read(stream_,
                         buffer_,
                         req_,
                         beast::bind_front_handler(
                                 &SimpleHttpServer::on_read,
                                 shared_from_this()));
    }

    void on_read(beast::error_code ec,
                 std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        if (ec == http::error::end_of_stream)
            //连接被关闭,一般是用户端关闭了连接
            return do_close();
        if (ec)
            return fail(ec, "on_read");
        //成功读取请求后，对请求做处理，下一步的异步处理在lambda中进行递交
        handle_request(*doc_root_, std::move(req_), lambda_);
    }

    void on_write(bool close,
                  beast::error_code ec,
                  std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);
        if (ec)
            return fail(ec, "on_write");
        if (close) {
            //连接关闭，一般是响应体中有该信号
            return do_close();
        }
        //若未关闭，清空响应体并准备接收下一次请求
        res_ = nullptr;
        do_read();
    }

    void do_close() {
        beast::error_code ec;
        //踢掉连接
        stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    }

};

//创建acceptor监听连接并启动上面的sessions
class SimpleHttpServerListener : public std::enable_shared_from_this<SimpleHttpServerListener> {
private:
    net::io_context &ioc_;
    tcp::acceptor acceptor_;
    std::shared_ptr<std::string const> doc_root_;
public:
    SimpleHttpServerListener(net::io_context &ioc,
                             tcp::endpoint endpoint,
                             std::shared_ptr<std::string const> doc_root
    ) : ioc_(ioc),
        acceptor_(net::make_strand(ioc)),
        doc_root_(std::move(doc_root)) {
        //监听初始化
        beast::error_code ec;
        //开启acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            fail(ec, "open");
            return;
        }
        //配置可重用
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            fail(ec, "set_option");
            return;
        }
        //绑定监听地址
        acceptor_.bind(endpoint, ec);
        if (ec) {
            fail(ec, "bind");
            return;
        }
        //开始监听
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            fail(ec, "listen");
            return;
        }
    }

    void run() {
        do_accept();//做接收到连接后的处理
    }

private:
    void do_accept() {
        acceptor_.async_accept(
                net::make_strand(ioc_),
                beast::bind_front_handler(
                        &SimpleHttpServerListener::on_accept,
                        shared_from_this()));
    }

    void on_accept(beast::error_code ec,
                   tcp::socket socket) {
        if (ec) {
            fail(ec, "accept");
        } else {
            std::make_shared<SimpleHttpServer>(
                    std::move(socket),
                    doc_root_
            )->run();
        }
        do_accept();
    }
};


#endif //CPPLEARNINGDEMOS_SIMPLEHTTPSERVER_H
