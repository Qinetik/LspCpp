//
// server.cpp

#include "WebSocketServer.h"

#include <iostream>
#include <signal.h>
#include <utility>
#include "MessageIssue.h"
#include "stream.h"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/dispatch.hpp>
namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>
namespace lsp {
    class websocket_stream_wrapper :public istream, public ostream
    {
    public:

        websocket_stream_wrapper(boost::beast::websocket::stream<boost::beast::tcp_stream>& _w);

        boost::beast::websocket::stream<boost::beast::tcp_stream>& ws_;
        std::atomic<bool> quit{};
        std::shared_ptr < MultiQueueWaiter> request_waiter;
        ThreadedQueue< char > on_request;
        bool fail()
        {
            return  bad();
        }
        bool eof()
        {
            return  bad();
        }
        bool good()
        {
            return  !bad();
        }
        websocket_stream_wrapper& read(char* str, std::streamsize count)
        {
            auto some = on_request.TryDequeueSome(count);
            size_t i = 0;
            for (; i < some.size(); ++i)
            {
                str[i] = some[i];
            }
            for (; i < count; ++i)
            {
                str[i] = static_cast<char>(get());
            }
            return  *this;
        }
        int get()
        {
            return on_request.Dequeue();
        }

        bool bad()
        {
            return !ws_.next_layer().socket().is_open();
        }

        websocket_stream_wrapper& websocket_stream_wrapper::write(const std::string& c)
        {
            ws_.write(boost::asio::buffer(std::string(c)));
            return *this;
        }

        websocket_stream_wrapper& websocket_stream_wrapper::write(std::streamsize _s)
        {
            std::ostringstream temp;
            temp << _s;
            ws_.write(boost::asio::buffer(temp.str()));
            return *this;
        }

        websocket_stream_wrapper& websocket_stream_wrapper::flush()
        {
            return *this;
        }
    };

    // Echoes back all received WebSocket messages
    class server_session : public std::enable_shared_from_this<server_session>
    {
        websocket::stream<beast::tcp_stream> ws_;
       
        beast::flat_buffer buffer_;
        std::string user_agent_;
    public:
       std::shared_ptr<websocket_stream_wrapper>  proxy_ =  std::make_shared<websocket_stream_wrapper>(ws_) ;
        // Take ownership of the socket
        explicit
            server_session(tcp::socket&& socket,const std::string& user_agent)
            : ws_(std::move(socket)),user_agent_(user_agent)
        {
        }

        // Get on the correct executor
        void
            run()
        {
            // We need to be executing within a strand to perform async operations
            // on the I/O objects in this server_session. Although not strictly necessary
            // for single-threaded contexts, this example code is written to be
            // thread-safe by default.
            net::dispatch(ws_.get_executor(),
                beast::bind_front_handler(
                    &server_session::on_run,
                    shared_from_this()));
        }

        // Start the asynchronous operation
        void
            on_run()
        {
            // Set suggested timeout settings for the websocket
            ws_.set_option(
                websocket::stream_base::timeout::suggested(
                    beast::role_type::server));

            // Set a decorator to change the Server of the handshake
            ws_.set_option(websocket::stream_base::decorator(
                [=](websocket::response_type& res)
                {
                    res.set(http::field::server, user_agent_.c_str());
                }));
            // Accept the websocket handshake
            ws_.async_accept(
                beast::bind_front_handler(
                    &server_session::on_accept,
                    shared_from_this()));
        }

        void
            on_accept(beast::error_code ec)
        {
            if (ec)
                return ;

            // Read a message
            // Read a message into our buffer
            ws_.async_read(
                buffer_,
                beast::bind_front_handler(
                    &server_session::on_read,
                    shared_from_this()));
        }

    

        void
            on_read(
                beast::error_code ec,
                std::size_t bytes_transferred)
        {
           

            // This indicates that the server_session was closed
            if (ec == websocket::error::closed)
                return;

            if (ec)
                return;
            char* data = reinterpret_cast<char*>(buffer_.data().data());
            std::vector<char> elements(data, data + bytes_transferred);
           
            buffer_.clear();
            proxy_->on_request.EnqueueAll(std::move(elements), false);
        	
            // Read a message into our buffer
            ws_.async_read(
                buffer_,
                beast::bind_front_handler(
                    &server_session::on_read,
                    shared_from_this()));
        }


        
        void close()
        {
        	if(ws_.is_open())
			 ws_.close(websocket::close_code::normal);
        }
    };

    //------------------------------------------------------------------------------

	    struct WebSocketServerData
	    {
		    WebSocketServerData(const std::string& user_agent, lsp::Log& log) :
			    acceptor_(io_context_), user_agent_(user_agent), _log(log)
		
		    {
		    }

	    	~WebSocketServerData()
		    {
              
		    }
            /// The io_context used to perform asynchronous operations.
            boost::asio::io_context io_context_;

            std::shared_ptr<boost::asio::io_service::work> work;

            /// Acceptor used to listen for incoming connections.
            boost::asio::ip::tcp::acceptor acceptor_;

            std::shared_ptr < server_session> _server_session;
         
            lsp::Log& _log;
            std::string user_agent_;
     
	    };

    websocket_stream_wrapper::websocket_stream_wrapper(boost::beast::websocket::stream<boost::beast::tcp_stream>& _w):
	    ws_(_w), request_waiter(new MultiQueueWaiter()),
	    on_request(request_waiter)
    {
    }

   


    WebSocketServer::~WebSocketServer()
	    {
            delete d_ptr;
	    }

        WebSocketServer::WebSocketServer(const std::string& user_agent, const std::string& address, const std::string& port,
            std::shared_ptr < MessageJsonHandler> json_handler,
            std::shared_ptr < Endpoint> localEndPoint, lsp::Log& log, uint32_t _max_workers)
            : remote_end_point_(json_handler,localEndPoint,log, _max_workers),d_ptr(new WebSocketServerData(user_agent,log))
           
        {
           
            d_ptr->work = std::make_shared<boost::asio::io_service::work>(d_ptr->io_context_);

            // Open the acceptor with the option to reuse the address (i.e. SO_REUSEADDR).
            boost::asio::ip::tcp::resolver resolver(d_ptr->io_context_);
            boost::asio::ip::tcp::endpoint endpoint =
                *resolver.resolve(address, port).begin();
            d_ptr->acceptor_.open(endpoint.protocol());
            d_ptr->acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            try
            {
                d_ptr->acceptor_.bind(endpoint);
            }
            catch (boost::system::system_error & e)
            {
                std::string temp = "Socket Server  blid faild.";
                d_ptr->_log.log(lsp::Log::Level::INFO , temp + e.what());
                return;
            }
            d_ptr->acceptor_.listen();

            do_accept();
            std::string desc = "Socket WebSocketServer " + address + " " + port + " start.";
            d_ptr->_log.log(lsp::Log::Level::INFO, desc);
        }

        void WebSocketServer::run()
        {
            // The io_context::run() call will block until all asynchronous operations
            // have finished. While the WebSocketServer is running, there is always at least one
            // asynchronous operation outstanding: the asynchronous accept call waiting
            // for new incoming connections.
            d_ptr->io_context_.run();

        }

        void WebSocketServer::stop()
        {
            try
            {
            	if(d_ptr->work)
                    d_ptr->work.reset();

                do_stop();
            }
            catch (...)
            {
            }
        }

        void WebSocketServer::do_accept()
        {
            d_ptr->acceptor_.async_accept(
                [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket)
                {
                    // Check whether the WebSocketServer was stopped by a signal before this
                    // completion handler had a chance to run.
                    if (!d_ptr->acceptor_.is_open())
                    {
                        return;
                    }

                    if (!ec)
                    {
                    	if(d_ptr->_server_session)
                    	{
                    		
						   d_ptr->_server_session->close();
                            remote_end_point_.Stop();
                    	}
                        d_ptr->_server_session = std::make_shared<server_session>(std::move(socket), d_ptr->user_agent_);
                        d_ptr->_server_session->run();
          
                        remote_end_point_.startProcessingMessages(d_ptr->_server_session->proxy_, d_ptr->_server_session->proxy_);
                        do_accept();
                    }

                   
                });
        }

        void WebSocketServer::do_stop()
        {
            d_ptr->acceptor_.close();
           
          remote_end_point_.Stop();
            
        }

    } // namespace 

