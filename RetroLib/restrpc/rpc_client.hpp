#pragma once
#include <string>
#include <deque>
#include <future>
#include "use_asio.hpp"
#include "client_util.hpp"
#include "const_vars.h"
#include "meta_util.hpp"

using namespace rest_rpc::rpc_service;

namespace rest_rpc {
	class req_result {
	public:
		req_result() = default;
		req_result(string_view data) : data_(data.data(), data.length()) {}
		bool success() const {
			return !has_error(data_);
		}

		template<typename T>
		T as() {
			if (has_error(data_)) {
				throw std::logic_error("rpc error");
			}

			return get_result<T>(data_);
		}

		void as() {
			if (has_error(data_)) {
				throw std::logic_error("rpc error");
			}
		}

	private:
		std::string data_;
	};

	enum class CallModel {
		future,
		callback
	};
	const constexpr auto FUTURE = CallModel::future;

	const constexpr size_t DEFAULT_TIMEOUT = 5000; //XXX modify this if necessary
	//to wait tag_stop stops.

	class rpc_client : private asio::noncopyable {
	public:
		rpc_client() : socket_(ios_), work_(ios_), 
			deadline_(ios_), body_(INIT_BUF_SIZE) {
			thd_ = std::make_shared<std::thread>([this] {
				ios_.run();
			});
		}

		rpc_client(const std::string& host, unsigned short port) : socket_(ios_), work_(ios_), 
			deadline_(ios_), host_(host), port_(port), body_(INIT_BUF_SIZE) {
			thd_ = std::make_shared<std::thread>([this] {
				ios_.run();
			});
		}

		~rpc_client() {
			close();
			stop();
		}

		void run(){
            thd_->join();
		}

		void set_connect_timeout(size_t milliseconds) {
			connect_timeout_ = milliseconds;
		}

		void set_reconnect_count(int reconnect_count) {
			reconnect_cnt_ = reconnect_count;
		}

		void set_wait_timeout(size_t seconds) {
			wait_timeout_ = seconds;
		}

		void async_connect() {
			assert(port_ != 0);
			auto addr = boost::asio::ip::address::from_string(host_);
			socket_.async_connect({ addr, port_ }, [this](const boost::system::error_code& ec) {
				if (has_connected_) {
					return;
				}

				if (ec) {
					//std::cout << ec.message() << std::endl;

					has_connected_ = false;

					if (reconnect_cnt_ == 0) {
						return;
					}

					if (reconnect_cnt_ > 0) {
						reconnect_cnt_--;
					}

                    async_reconnect();
				}
				else {
					has_connected_ = true;
					do_read();
					if(has_wait_)
					    conn_cond_.notify_one();
				}
			});
		}

		void async_reconnect(){
            reset_socket();
            async_connect();
			std::this_thread::sleep_for(std::chrono::milliseconds(connect_timeout_));
		}

		bool connect(size_t timeout = 1) {
			assert(port_ != 0);
			async_connect();
			return wait_conn(timeout);
		}

		bool connect(const std::string& host, unsigned short port, size_t timeout = 1) {
			if (port_==0) {
				host_ = host;
				port_ = port;
			}

			return connect(timeout);
		}

		void async_connect(const std::string& host, unsigned short port) {
			if (port_ == 0) {
				host_ = host;
				port_ = port;
			}

			async_connect();
		}

		bool wait_conn(size_t timeout) {
			if (has_connected_) {
				return true;
			}

            has_wait_ = true;
			std::unique_lock<std::mutex> lock(conn_mtx_);
			bool result = conn_cond_.wait_for(lock, std::chrono::seconds(timeout),
				[this] {return has_connected_; });
            has_wait_ = false;
			return has_connected_;
		}

		void update_addr(const std::string& host, unsigned short port) {
			host_ = host;
			port_ = port;
		}

		void close() {
			has_connected_ = false;
			if (socket_.is_open()) {
				boost::system::error_code ignored_ec;
				socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored_ec);
				socket_.close(ignored_ec);
			}
			clear_cache();
		}

		void set_error_callback(std::function<void(boost::system::error_code)> f) {
			err_cb_ = std::move(f);
		}

		//sync call
#if __cplusplus > 201402L
		template<size_t TIMEOUT, typename T = void, typename... Args>
		auto call(const std::string& rpc_name, Args&& ... args) {
			std::future<req_result> future = async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
			auto status = future.wait_for(std::chrono::milliseconds(TIMEOUT));
			if (status == std::future_status::timeout || status == std::future_status::deferred) {
				throw std::out_of_range("timeout or deferred");
			}

			if constexpr (std::is_void_v<T>) {
				future.get().as();
			}
			else {
				return future.get().as<T>();
			}
		}

		template<typename T = void, typename... Args>
		auto call(const std::string& rpc_name, Args&& ... args) {
			return call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
		}
#else
		template<size_t TIMEOUT, typename T=void, typename... Args>
		typename std::enable_if<std::is_void<T>::value>::type call(const std::string& rpc_name, Args&& ... args) {
			std::future<req_result> future = async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
			auto status = future.wait_for(std::chrono::milliseconds(TIMEOUT));
			if (status == std::future_status::timeout || status == std::future_status::deferred) {
				throw std::out_of_range("timeout or deferred");
			}

			future.get().as();
		}

		template<typename T = void, typename... Args>
		typename std::enable_if<std::is_void<T>::value>::type call(const std::string& rpc_name, Args&& ... args) {
			call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
		}

		template<size_t TIMEOUT, typename T, typename... Args>
		typename std::enable_if<!std::is_void<T>::value, T>::type call(const std::string& rpc_name, Args&& ... args) {
			std::future<req_result> future = async_call<FUTURE>(rpc_name, std::forward<Args>(args)...);
			auto status = future.wait_for(std::chrono::milliseconds(TIMEOUT));
			if (status == std::future_status::timeout || status == std::future_status::deferred) {
				throw std::out_of_range("timeout or deferred");
			}

			return future.get().as<T>();
		}

		template<typename T, typename... Args>
		typename std::enable_if<!std::is_void<T>::value, T>::type call(const std::string& rpc_name, Args&& ... args) {
			return call<DEFAULT_TIMEOUT, T>(rpc_name, std::forward<Args>(args)...);
		}
#endif

		template<CallModel model, typename... Args>
		std::future<req_result> async_call(const std::string& rpc_name, Args&&... args) {
			auto p = std::make_shared<std::promise<req_result>>();
			std::future<req_result> future = p->get_future();

			uint64_t fu_id = 0;
			{
				std::unique_lock<std::mutex> lock(cb_mtx_);
				fu_id_++;
				fu_id = fu_id_;
				future_map_.emplace(fu_id, std::move(p));
			}

			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, std::forward<Args>(args)...);
			write(fu_id, std::move(ret));
			return future;
		}

		template<size_t TIMEOUT = DEFAULT_TIMEOUT, typename... Args>
		void async_call(const std::string& rpc_name, std::function<void(boost::system::error_code, string_view)> cb, Args&& ... args) {
			uint64_t cb_id = 0;
			{
				std::unique_lock<std::mutex> lock(cb_mtx_);
				callback_id_++;
				callback_id_ |= (uint64_t(1) << 63);
				cb_id = callback_id_;
				auto call = std::make_shared<call_t>(ios_, std::move(cb), TIMEOUT);
				call->start_timer();
				callback_map_.emplace(cb_id, call);
			}

			msgpack_codec codec;
			auto ret = codec.pack_args(rpc_name, std::forward<Args>(args)...);
			write(cb_id, std::move(ret));
		}

		bool has_connected() const {
			return has_connected_;
		}

		void stop() {
			if (thd_ != nullptr) {
				ios_.stop();
				thd_->join();
				thd_ = nullptr;
			}
		}

	private:
		using message_type = std::pair<string_view, std::uint64_t>;
		void reset_deadline_timer(size_t timeout) {
			deadline_.expires_from_now(std::chrono::seconds(timeout));
			deadline_.async_wait([this](const boost::system::error_code& ec) {
				if (!ec) {
					socket_.close();
				}
			});
		}

		void write(std::uint64_t req_id, buffer_type&& message) {
			size_t size = message.size();
			assert(size < MAX_BUF_LEN);
			message_type msg{ {message.release(), size}, req_id };

			std::unique_lock<std::mutex> lock(write_mtx_);
			outbox_.emplace_back(std::move(msg));
			if (outbox_.size() > 1) {
				// outstanding async_write
				return;
			}

			write();
		}

		void write() {
			auto& msg = outbox_[0];
			write_size_ = (uint32_t)msg.first.length();
			std::array<boost::asio::const_buffer, 3> write_buffers;
			write_buffers[0] = boost::asio::buffer(&write_size_, sizeof(int32_t));
			write_buffers[1] = boost::asio::buffer(&msg.second, sizeof(uint64_t));
			write_buffers[2] = boost::asio::buffer((char*)msg.first.data(), write_size_);

			boost::asio::async_write(socket_, write_buffers,
				[this](const boost::system::error_code& ec, const size_t length) {
				if (ec) {
					has_connected_ = false;
					close();
					if (err_cb_) {
						err_cb_(ec);
					}

					return;
				}

				std::unique_lock<std::mutex> lock(write_mtx_);
				::free((char*)outbox_.front().first.data());
				outbox_.pop_front();

				if (!outbox_.empty()) {
					// more messages to send
					this->write();
				}
			});
		}

		void do_read() {
			boost::asio::async_read(socket_, boost::asio::buffer(head_),
				[this](const boost::system::error_code& ec, const size_t length) {
				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					has_connected_ = false;
					return;
				}

				if (!ec) {
					const uint32_t body_len = *((uint32_t*)(head_));
					auto req_id = *((std::uint64_t*)(head_ + sizeof(int32_t)));
					if (body_len > 0 && body_len < MAX_BUF_LEN) {
						if (body_.size() < body_len) { body_.resize(body_len); }
						read_body(req_id, body_len);
						return;
					}

					if (body_len == 0 || body_len > MAX_BUF_LEN) {
						//LOG(INFO) << "invalid body len";
                        close();
						if (err_cb_) {
							err_cb_(asio::error::make_error_code(asio::error::message_size));
						}
						return;
					}
				}
				else {
					//LOG(INFO) << ec.message();
					has_connected_ = false;
					close();
                    if (err_cb_) err_cb_(ec);
				}
			});
		}

		void read_body(std::uint64_t req_id, size_t body_len) {
			boost::asio::async_read(
				socket_, boost::asio::buffer(body_.data(), body_len),
				[this, req_id, body_len](boost::system::error_code ec, std::size_t length) {
				//cancel_timer();

				if (!socket_.is_open()) {
					//LOG(INFO) << "socket already closed";
					call_back(req_id, asio::error::make_error_code(asio::error::connection_aborted), {});
					return;
				}

				if (!ec) {
					//entier body
					call_back(req_id, ec, { body_.data(), body_len });

					do_read();
				}
				else {
					//LOG(INFO) << ec.message();
					has_connected_ = false;
                    close();
					if (err_cb_) {
						err_cb_(ec);
					}
				}
			});
		}

		void call_back(uint64_t req_id, const boost::system::error_code& ec, string_view data) {
			auto cb_flag = req_id >> 63;
			if (cb_flag) {
				std::shared_ptr<call_t> cl = nullptr;
				{
					std::unique_lock<std::mutex> lock(cb_mtx_);
					cl = std::move(callback_map_[req_id]);
				}

				assert(cl);
				if (!cl->has_timeout()) {
					cl->cancel();
					cl->callback(ec, data);
				}
				else {
					cl->callback(asio::error::make_error_code(asio::error::timed_out), {});
				}

				std::unique_lock<std::mutex> lock(cb_mtx_);
				callback_map_.erase(req_id);
			}
			else {
				std::unique_lock<std::mutex> lock(cb_mtx_);
				auto& f = future_map_[req_id];
				if (ec) {
					//LOG<<ec.message();
					if (!f) {
						//std::cout << "invalid req_id" << std::endl;
						return;
					}
				}

				assert(f);
				f->set_value(req_result{ data });
				future_map_.erase(req_id);
			}
		}

		void clear_cache() {
			{
				std::unique_lock<std::mutex> lock(write_mtx_);
				while (!outbox_.empty()) {
					::free((char*)outbox_.front().first.data());
					outbox_.pop_front();
				}
			}

			{
				std::unique_lock<std::mutex> lock(cb_mtx_);
				callback_map_.clear();
				future_map_.clear();
			}
		}

        void reset_socket(){
            boost::system::error_code igored_ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, igored_ec);
            socket_.close(igored_ec);
            socket_ = decltype(socket_)(ios_);
            if(!socket_.is_open()){
                socket_.open(boost::asio::ip::tcp::v4());
            }
        }

		class call_t : asio::noncopyable, public std::enable_shared_from_this<call_t> {
		public:
			call_t(asio::io_service& ios, std::function<void(boost::system::error_code, string_view)> cb, size_t timeout) : timer_(ios),
				cb_(std::move(cb)), timeout_(timeout){
			}

			void start_timer() {
				if (timeout_ == 0) {
					return;
				}

				timer_.expires_from_now(std::chrono::milliseconds(timeout_));
				auto self = this->shared_from_this();
				timer_.async_wait([this, self](boost::system::error_code ec) {
					if (ec) {
						return;
					}

					has_timeout_ = true;
				});
			}

			void callback(boost::system::error_code ec, string_view data) {
				cb_(ec, data);
			}

			bool has_timeout() const {
				return has_timeout_;
			}

			void cancel() {
				if (timeout_ == 0) {
					return;
				}

				boost::system::error_code ec;
				timer_.cancel(ec);
			}

		private:
			boost::asio::steady_timer timer_;
			std::function<void(boost::system::error_code, string_view)> cb_;
			size_t timeout_;
			bool has_timeout_ = false;
		};

		boost::asio::io_service ios_;
		asio::ip::tcp::socket socket_;
		boost::asio::io_service::work work_;
		std::shared_ptr<std::thread> thd_ = nullptr;

		std::string host_;
		unsigned short port_ = 0;
		size_t connect_timeout_ = 2000;//s
		size_t wait_timeout_ = 2;//s
		int reconnect_cnt_ = -1;
		bool has_connected_ = false;
		std::mutex conn_mtx_;
		std::condition_variable conn_cond_;
		bool has_wait_ = false;

		asio::steady_timer deadline_;

		std::deque<message_type> outbox_;
		uint32_t write_size_ = 0;
		std::mutex write_mtx_;
		uint64_t fu_id_ = 0;
		std::function<void(boost::system::error_code)> err_cb_;

		std::unordered_map<std::uint64_t, std::shared_ptr<std::promise<req_result>>> future_map_;
		std::unordered_map<std::uint64_t, std::shared_ptr<call_t>> callback_map_;
		std::mutex cb_mtx_;
		uint64_t callback_id_ = 0;

		char head_[HEAD_LEN] = {};
		std::vector<char> body_;
	};
}