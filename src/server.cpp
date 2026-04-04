
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <memory>
#include <random>
#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>

namespace beast = boost::beast;
namespace json = boost::json;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

/* DATABASE */
std::unordered_map<std::string, std::string> url_db;
std::mutex db_mutex;

/* RANDOM GENERATOR */
const std::string ALL_CHARACTERS = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
const std::string DOMAIN_HOST_NAME = "http://localhost:8080/";

std::string generateRandomCode(const int length = 10) {
	thread_local std::mt19937 rng(std::random_device{}());  // mt19937 is a random number generator, seeded with a random device for better randomness
	std::uniform_int_distribution<> dist(0, 61); // uniform distribution to get indices for ALL_CHARACTERS (0-61) since 62 chars are present

	std::string code;
	code.reserve(length);

	for (int i = 0; i < length; i++) {
		code += ALL_CHARACTERS[dist(rng)];
	}

	return code;
}

std::string generateUniqueCode() {
	std::string code;
	std::lock_guard lock(db_mutex);

	do {
		code = generateRandomCode(10);
	} while (url_db.contains(code)); // if count returns 0, it means code is not found - breaks loop

	return code;
}

/*** HTTP SERVER
 * - POST /api/shorten: Accepts JSON payload with "url" field, generates a short code, stores the mapping, and returns the shortened URL.
 * - GET /api/expand/<code>: Returns the original URL for a given short code in JSON format.
 * - GET /api/code/<code>: Redirects to the original URL associated with the short code.
 */


/* SESSION */
class session : public std::enable_shared_from_this<session> {
	tcp::socket socket_;
	beast::flat_buffer buffer_;
	http::request<http::string_body> req_;

public:
	explicit session(tcp::socket socket)
		: socket_(std::move(socket)) {}

	void start() {
		do_read();
	}
private:
	void do_read() {
		auto self = shared_from_this();

		http::async_read(socket_, buffer_, req_,
			[self](const beast::error_code &ec, std::size_t) {
				if (!ec)
					self->handle_request();
			});
	}

	void handle_request() {
        const auto res = std::make_shared<http::response<http::string_body>>();
		beast::string_view host_value = req_.base().find(http::field::host)->value();

        res->set(http::field::server, "Async Beast");
        res->set(http::field::access_control_allow_origin, "*");
        res->set(http::field::content_type, "application/json");

        // -------- POST /api/shorten --------
        if (req_.method() == http::verb::post && req_.target() == "/api/shorten") {
            const std::string req_payload = req_.body();
        	json::value jv = json::parse(req_payload);

        	auto& obj = jv.as_object();
        	if (!obj.contains("url") || !obj["url"].is_string()) {
        		res->result(http::status::bad_request);
				res->body() = R"({"error":"Invalid JSON payload"})";
				res->prepare_payload();
				do_write(res);
				return;
			}

			const std::string longUrl = obj["url"].as_string().c_str();
            const std::string code = generateUniqueCode();

            std::lock_guard lock(db_mutex);
            url_db[code] = longUrl;

            const std::string shortUrl = std::string(host_value)+"/api/code/" + code;

            res->result(http::status::ok);
            res->body() = R"({"shortUrl":")" + shortUrl + "\"}";
        }

        // -------- GET /api/expand/<code> --------
        else if (req_.method() == http::verb::get &&
                 req_.target().starts_with("/api/expand/")) {

            const std::string code = std::string(req_.target()).substr(12);

            std::lock_guard lock(db_mutex);

            if (url_db.contains(code)) {
                res->result(http::status::ok);
                res->body() = R"({"longUrl":")" + url_db[code] + "\"}";
            } else {
                res->result(http::status::not_found);
                res->body() = R"({"error":"Not found"})";
            }
        }

        // -------- GET /api/code/<code> (redirect) --------
        else if (req_.method() == http::verb::get && req_.target().starts_with("/api/code/")) {
            const std::string code = std::string(req_.target()).substr(10);

            std::lock_guard lock(db_mutex);

            if (url_db.contains(code)) {
                res->result(http::status::found);
                res->set(http::field::location, url_db[code]);
            } else {
                res->result(http::status::not_found);
                res->body() = "Not Found";
            }
        }

		// -------- GET /api/health --------
		else if (req_.method() == http::verb::get && req_.target().starts_with("/api/health")) {
			res->result(http::status::ok);
			res->body() = R"({"status":"OK"})";
		}

        else {
            res->result(http::status::bad_request);
            res->body() = R"({"error":"Invalid request"})";
        }

        res->prepare_payload();
        do_write(res);
    }
	
	void do_write(const std::shared_ptr<http::response<http::string_body>> &res) {
        auto self = shared_from_this();

        http::async_write(socket_, *res,
            [self, res](beast::error_code ec, std::size_t) {
                self->socket_.shutdown(tcp::socket::shutdown_send, ec);
            });
    }
};

/* LISTENER */
class listener : public std::enable_shared_from_this<listener> {
	tcp::acceptor acceptor_;
	tcp::socket socket_;

public:
	listener(net::io_context& ioc, const tcp::endpoint &endpoint)
		: acceptor_(ioc), socket_(ioc) {
		acceptor_.open(endpoint.protocol());
		acceptor_.set_option(net::socket_base::reuse_address(true));
		acceptor_.bind(endpoint);
		acceptor_.listen();
	}

	void run() {
		do_accept();
	}

private:
	void do_accept() {
		auto self = shared_from_this();

		acceptor_.async_accept(socket_,
			[self](beast::error_code ec) {
				if (!ec) {
					std::make_shared<session>(std::move(self->socket_))->start();
				}
				self->do_accept();
			});
	}
};

int main() {
	try {
		net::io_context ioc{1};

		auto server = std::make_shared<listener>(
			ioc, tcp::endpoint(tcp::v4(), 8080)
		);
		server->run();

		std::cout << "Server running on " << DOMAIN_HOST_NAME << "\n";

		std::vector<std::thread> threads;
		const unsigned int n = std::thread::hardware_concurrency();
		threads.reserve(n);
		for (int i = 0; i < n; i++) {
			threads.emplace_back([&ioc]() {
				ioc.run();
			});
		}

		for (auto& t : threads)
			t.join();

	} catch (std::exception& e) {
		std::cerr << e.what() << std::endl;
	}
}