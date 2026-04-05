
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/redis/src.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <boost/url.hpp>
#include <memory>
#include <random>
#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <libpq-fe.h>

namespace redis = boost::redis;
namespace beast = boost::beast;
namespace json = boost::json;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

/* RANDOM GENERATOR */
const std::string ALL_CHARACTERS = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
const std::string DOMAIN_HOST_NAME = "http://localhost:8080/";

const std::string REDIS_HOST = "127.0.0.1";
const std::string REDIS_PORT = "6379";

const std::string POSTGRES_HOST = "localhost";
const std::string POSTGRES_PORT = "5432";
const std::string POSTGRES_DB_NAME = "url_shortener_db";
const std::string POSTGRES_USER = "postgres";
const std::string POSTGRES_PASSWORD = "postgres";

net::thread_pool pool(4); // Threadpool for DB connections

struct DBResult {
	std::string site;
	int count;
};

struct RedisResult {
	std::string long_url;
	long long ttl;
};

void initialise_postgres(const std::shared_ptr<PGconn>& conn) {
	if (PQstatus(conn.get()) != CONNECTION_OK) {
		std::cerr << "Connection to database failed: " << PQerrorMessage(conn.get()) << std::endl;
		return;
	}
	const auto create_table_query = "CREATE TABLE IF NOT EXISTS url_shortener (id INTEGER GENERATED ALWAYS AS IDENTITY, site TEXT NOT NULL UNIQUE, count INTEGER DEFAULT 1, PRIMARY KEY (id));";
	PQexec(conn.get(), create_table_query);
}

// 2. Modify the method to be an awaitable
net::awaitable<void> async_insert_postgres(
	const std::shared_ptr<PGconn> conn,  const std::string& host) {

	// Switch to the thread pool to perform the blocking DB call
	co_await net::post(pool, net::use_awaitable);

	const char* params[] = {host.c_str()};
	PGresult* res = PQexecParams(conn.get(),
		"INSERT INTO url_shortener (site) VALUES ($1) ON CONFLICT (site) "
		"DO UPDATE SET count = url_shortener.count + 1;",
		1, nullptr, params, nullptr, nullptr, 0);

	if (PQresultStatus(res) == PGRES_COMMAND_OK) {
		std::cout << "Insert successful on background thread" << std::endl;
	}
	PQclear(res);

	// Switch back to the original executor (the event loop)
	co_await net::post(co_await net::this_coro::executor, net::use_awaitable);
}

std::vector<DBResult> get_top10_results_from_db(const std::shared_ptr<PGconn>& conn) {
	std::vector<DBResult> results;
	PGresult* res = PQexec(conn.get(), "SELECT site, count FROM url_shortener ORDER BY count DESC LIMIT 10;");
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		const int rows = PQntuples(res);
		for (int i = 0; i < rows; i++) {
			results.push_back({PQgetvalue(res, i, 0), std::stoi(PQgetvalue(res, i, 1))});
		}
	}
	PQclear(res);
	return results;
}

net::awaitable<std::optional<RedisResult>> get_code_from_redis(const std::shared_ptr<redis::connection>& conn, const std::string& code) {
	redis::request req;
	req.push("GET", code);
	req.push("TTL", code);

	// Response maps to the order of pushed commands
	// Command 1 (GET): std::optional<std::string>
	// Command 2 (TTL): long long (Redis integer)
	redis::response<std::optional<std::string>, long long> resp;
	co_await conn->async_exec(req, resp, net::use_awaitable);

	auto const& value = std::get<0>(resp).value();
	if (const auto ttl = std::get<1>(resp).value(); value && ttl) co_return RedisResult{*value, ttl};

	// If we reach here, it means the code was not found in Redis
	co_return std::nullopt;
}


std::string generate_random_code(const int length = 10) {
	thread_local std::mt19937 rng(std::random_device{}());  // mt19937 is a random number generator, seeded with a random device for better randomness
	std::uniform_int_distribution<> dist(0, 61); // uniform distribution to get indices for ALL_CHARACTERS (0-61) since 62 chars are present

	std::string code;
	code.reserve(length);

	for (int i = 0; i < length; i++) {
		code += ALL_CHARACTERS[dist(rng)];
	}

	return code;
}

net::awaitable<bool> code_exists_in_redis(const std::shared_ptr<redis::connection> conn, std::string code) {
	redis::request req;
	req.push("EXISTS", code);

	redis::response<int> resp;

	// This yields the thread back to the io_context until the result is ready
	co_await conn->async_exec(req, resp, net::use_awaitable);

	// Extract the value safely
	auto const& result = std::get<0>(resp);
	co_return result.has_value() && result.value() == 1; // Redis returns 1 if the key exists, 0 if it does not
}

net::awaitable<std::string> generate_unique_code(std::shared_ptr<redis::connection> conn) {
	std::string code;
	bool exists = true;

	do {
		code = generate_random_code(10);
		exists = co_await code_exists_in_redis(conn, code);

	} while (exists); // Loop until code_exists_in_redis returns false

	co_return code;
}

net::awaitable<void> store_with_expiry(const std::shared_ptr<redis::connection> &conn, const std::string& key, const std::string& value, int expiry_seconds) {
	// 1. Create a request object
	redis::request req;

	// 2. Use SET with the "EX" option (expiration in seconds)
	// Format: SET key value EX seconds
	req.push("SET", key, value, "EX", std::to_string(expiry_seconds));

	// 3. Prepare a response object (expecting a "OK" string for SET)
	redis::response<std::string> resp;

	// 4. Execute the request asynchronously
	co_await conn->async_exec(req, resp, net::use_awaitable);

	if (std::get<0>(resp).value() != "OK") {
		std::cerr << "Failed to store key in Redis: " << std::get<0>(resp).value() << std::endl;
	}
}

/*** HTTP SERVER
 * - POST /api/shorten: Accepts JSON payload with "url" field, generates a short code, stores the mapping, and returns the shortened URL.
 * - GET /api/expand/<code>: Returns the original URL for a given short code in JSON format.
 * - GET /api/code/<code>: Redirects to the original URL associated with the short code.
 * - GET /api/health: Returns the service health with status OK. (for probing container liveness and readiness checks)
 * - GET /api/stats: Returns the top 10 most frequently shortened domains
 */


/* SESSION */
class session : public std::enable_shared_from_this<session> {
	tcp::socket socket_;
	beast::flat_buffer buffer_;
	http::request<http::string_body> req_;
	std::shared_ptr<redis::connection> redis_conn_;
	std::shared_ptr<PGconn> pg_conn_;

public:
	explicit session(tcp::socket socket, const std::shared_ptr<redis::connection> &conn, const std::shared_ptr<PGconn>& pg_conn)
		: socket_(std::move(socket)), redis_conn_(conn), pg_conn_(pg_conn) {
	}

	void start() {
		auto self = shared_from_this();
		http::async_read(socket_, buffer_, req_,
			[self](const beast::error_code &ec, std::size_t) {
				if (!ec) {
					// Wrap in a lambda to manage lifetime correctly
					net::co_spawn(self->socket_.get_executor(),
						[self]() -> net::awaitable<void> {
							co_await self->handle_request();
						},
						net::detached);
				}
			});
	}
private:
	net::awaitable<void> handle_request() {
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
        		co_await do_write(res);
				co_return;
			}

			const std::string longUrl = obj["url"].as_string().c_str();
            const std::string code = co_await generate_unique_code(redis_conn_);

        	// Store in Redis with 24 hours expiry
        	co_await store_with_expiry(redis_conn_, code, longUrl, 1 * 3600);

        	// parse the url to extract the domain and store in DB
            if (const auto result = boost::urls::parse_uri(longUrl); result.has_value()) {
        		const boost::urls::url_view& u = *result;
            	std::string host = u.host();

            	// auto sd = net::posix::stream_descriptor(socket_.get_executor(), fileno(stdin));
            	co_await async_insert_postgres(pg_conn_, host);
        	}

            const std::string shortUrl = std::string(host_value)+"/api/code/" + code;

            res->result(http::status::ok);
            res->body() = R"({"shortUrl":")" + shortUrl + "\"}";
        }

        // -------- GET /api/expand/<code> --------
        else if (req_.method() == http::verb::get &&
                 req_.target().starts_with("/api/expand/")) {

            const std::string code = std::string(req_.target()).substr(12);

            std::optional<RedisResult> redis_result = co_await get_code_from_redis(redis_conn_, code);

            if (redis_result.has_value()) {
                res->result(http::status::ok);
                res->body() = R"({"longUrl":")" + redis_result.value().long_url + R"(", "expiresInSeconds": )" + std::to_string(redis_result.value().ttl) + "}";
            } else {
                res->result(http::status::not_found);
                res->body() = R"({"error":"Not found"})";
            }
        }

        // -------- GET /api/code/<code> (redirect) --------
        else if (req_.method() == http::verb::get && req_.target().starts_with("/api/code/")) {
            const std::string code = std::string(req_.target()).substr(10);
        	std::optional<RedisResult> redis_result = co_await get_code_from_redis(redis_conn_, code);

            if (redis_result.has_value()) {
                res->result(http::status::found);
                res->set(http::field::location, redis_result.value().long_url);
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

		// -------- GET /api/stats --------
		else if (req_.method() == http::verb::get && req_.target().starts_with("/api/stats")) {
			std::vector<DBResult> top10 = get_top10_results_from_db(pg_conn_);
			json::array arr;
			for (const auto& entry : top10) {
				json::object obj;
				obj["site"] = entry.site;
				obj["count"] = entry.count;
				arr.push_back(obj);
			}
			json::object response;
			response["top10"] = arr;
			res->result(http::status::ok);
			res->body() = json::serialize(response);
		}

        else {
            res->result(http::status::bad_request);
            res->body() = R"({"error":"Invalid request"})";
        }

        res->prepare_payload();
        co_await do_write(res);
    }
	
	net::awaitable<void> do_write(const std::shared_ptr<http::response<http::string_body>> res) {
		co_await http::async_write(socket_, *res, net::use_awaitable);
		beast::error_code ec;
		socket_.shutdown(tcp::socket::shutdown_send, ec);
	}
};

/* LISTENER */
class listener : public std::enable_shared_from_this<listener> {
	tcp::acceptor acceptor_;
	tcp::socket socket_;
	std::shared_ptr<redis::connection> redis_conn_;
	std::shared_ptr<PGconn> pg_conn_;

public:
	listener(net::io_context& ioc, const tcp::endpoint &endpoint, const std::shared_ptr<redis::connection>& conn, const std::shared_ptr<PGconn>& pg_conn)
		: acceptor_(ioc), socket_(ioc), redis_conn_(conn), pg_conn_(pg_conn) {
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
					std::make_shared<session>(std::move(self->socket_), self->redis_conn_, self->pg_conn_)->start();
				}
				self->do_accept();
			});
	}
};

int main() {
	try {
		net::io_context ioc{1};

		/* Redis connection */
		const auto redis_conn = std::make_shared<redis::connection>(ioc.get_executor());

		redis::config cfg;
		cfg.addr = {REDIS_HOST, REDIS_PORT};
		// Run with the config defined above
		net::co_spawn(ioc, [redis_conn, cfg]() -> net::awaitable<void> {
			co_await redis_conn->async_run(cfg, {}, net::use_awaitable);
		}, net::detached);

		/* Postgres initialization */
		const std::string conninfo = "user=" + POSTGRES_USER + " password=" + POSTGRES_PASSWORD +
					   " dbname=" + POSTGRES_DB_NAME + " host=" + POSTGRES_HOST +
					   " port=" + POSTGRES_PORT;

		// Create the shared_ptr with PQfinish as the custom deleter
		std::shared_ptr<PGconn> pg_conn(PQconnectdb(conninfo.c_str()), PQfinish);
		initialise_postgres(pg_conn);


		const auto server = std::make_shared<listener>(
			ioc, tcp::endpoint(tcp::v4(), 8080), redis_conn, pg_conn
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