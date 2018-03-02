#include <iostream>
#include <cstdint>
#include <random>

#include <restinio/all.hpp>

#include <clara.hpp>

#include <fmt/format.h>

using std::chrono::milliseconds;

// Конфигурация, которая потребуется серверу.
struct config_t {
	// Адрес, на котором нужно слушать новые входящие запросы.
	std::string address_{"localhost"};
	// Порт, на котором нужно слушать.
	std::uint16_t port_{8090};

	// Минимальная величина задержки перед выдачей ответа.
	milliseconds min_pause_{4000};
	// Максимальная величина задержки перед выдачей ответа.
	milliseconds max_pause_{6000};

	// Нужно ли включать трассировку?
	bool tracing_{false};
};

// Разбор аргументов командной строки.
// В случае неудачи порождается исключение.
auto parse_cmd_line_args(int argc, char ** argv) {
	struct result_t {
		bool help_requested_{false};
		config_t config_;
	};
	result_t result;
	long min_pause{result.config_.min_pause_.count()};
	long max_pause{result.config_.max_pause_.count()};

	// Подготавливаем парсер аргументов командной строки.
	using namespace clara;

	auto cli = Opt(result.config_.address_, "address")["-a"]["--address"]
				("address to listen (default: localhost)")
		| Opt(result.config_.port_, "port")["-p"]["--port"]
				("port to listen (default: 8090)")
		| Opt(min_pause, "minimal pause")["-m"]["--min-pause"]
				("minimal pause before response, milliseconds")
		| Opt(max_pause, "maximum pause")["-M"]["--max-pause"]
				("maximal pause before response, milliseconds")
		| Opt(result.config_.tracing_)["-t"]["--tracing"]
				("turn server tracing ON (default: OFF)")
		| Help(result.help_requested_);

	// Выполняем парсинг...
	auto parse_result = cli.parse(Args(argc, argv));
	// ...и бросаем исключение если столкнулись с ошибкой.
	if(!parse_result)
		throw std::runtime_error("Invalid command line: "
				+ parse_result.errorMessage());

	if(result.help_requested_)
		std::cout << cli << std::endl;
	else {
		// Некоторые аргументы нуждаются в дополнительной проверке.
		if(min_pause <= 0)
			throw std::runtime_error("minimal pause can't be less or equal to 0");
		if(max_pause <= 0)
			throw std::runtime_error("maximal pause can't be less or equal to 0");
		if(max_pause < min_pause)
			throw std::runtime_error("minimal pause can't be less than "
					"maximum pause");

		result.config_.min_pause_ = milliseconds{min_pause};
		result.config_.max_pause_ = milliseconds{max_pause};
	}

	return result;
}

// Вспомогательный тип для генерации случайных задержек.
class pauses_generator_t {
	std::mt19937 generator_{std::random_device{}()};
	std::uniform_int_distribution<long> distrib_;
	const milliseconds minimal_;
public:
	pauses_generator_t(milliseconds min, milliseconds max)
		:	distrib_{0, (max - min).count()}
		,	minimal_{min}
		{}

	auto next() {
		return minimal_ + milliseconds{distrib_(generator_)};
	}
};

// Реализация обработчика запросов.
restinio::request_handling_status_t handler(
		restinio::asio_ns::io_context & ioctx,
		pauses_generator_t & generator,
		restinio::request_handle_t req) {
	// Выполняем задержку на случайную величину (но в заданных пределах).
	const auto pause = generator.next();
	// Для отсчета задержки используем Asio-таймеры.
	auto timer = std::make_shared<restinio::asio_ns::steady_timer>(ioctx);
	timer->expires_after(pause);
	timer->async_wait([timer, req, pause](const auto & ec) {
			if(!ec) {
				// Таймер успешно сработал, можно генерировать ответ.
				req->create_response()
					.append_header(restinio::http_field::server, "RESTinio hello world server")
					.append_header_date_field()
					.append_header(restinio::http_field::content_type, "text/plain; charset=utf-8")
					.set_body(
						fmt::format("Hello world!\nPause: {}ms.\n", pause.count()))
					.done();
			}
		} );

	// Подтверждаем, что мы приняли запрос к обработке и что когда-то
	// мы ответ сгенерируем.
	return restinio::request_accepted();
}

// Мы будем использовать express-router. Для простоты определяем псевдоним
// для нужного типа.
using express_router_t = restinio::router::express_router_t<>;

// Так же нам потребуются два вспомогательных типа свойств для http-сервера.

// Первый тип для случая, когда трассировка сервера не нужна.
struct non_traceable_server_traits_t : public restinio::default_single_thread_traits_t {
	using request_handler_t = express_router_t;
};

// Второй тип для случая, когда трассировка сервера нужна.
struct traceable_server_traits_t : public restinio::default_single_thread_traits_t {
	using request_handler_t = express_router_t;
	using logger_t = restinio::single_threaded_ostream_logger_t;
};

// Вспомогательная функция, которая отвечает за запуск сервера нужного типа.
template<typename Server_Traits, typename Handler>
void run_server(
		restinio::asio_ns::io_context & ioctx,
		const config_t & config,
		Handler && handler) {
	// Сперва создадим и настроим объект express-роутера.
	auto router = std::make_unique<express_router_t>();
	// Вот этот URL мы готовы обрабатывать.
	router->http_get(
			R"(/:year(\d{4})/:month(\d{2})/:day(\d{2}))",
			std::forward<Handler>(handler));
	// На все остальное будем отвечать 404.
	router->non_matched_request_handler([](auto req) {
			return req->create_response(404, "Not found")
					.append_header_date_field()
					.connection_close()
					.done();
		});

	restinio::run(ioctx,
			restinio::on_this_thread<Server_Traits>()
				.address(config.address_)
				.port(config.port_)
				.handle_request_timeout(config.max_pause_)
				.request_handler(std::move(router)));
}

int main(int argc, char ** argv) {
	try {
		const auto cfg = parse_cmd_line_args(argc, argv);
		if(cfg.help_requested_)
			return 1;

		// Нам нужен собственный io_context для того, чтобы мы могли с ним
		// работать напрямую в обработчике запросов.
		restinio::asio_ns::io_context ioctx;

		// Так же нам потребуется генератор случайных задержек в выдаче ответов.
		pauses_generator_t generator{cfg.config_.min_pause_, cfg.config_.max_pause_};

		// Нам нужен обработчик запросов, который будет использоваться
		// вне зависимости от того, какой именно сервер мы будем запускать
		// (с трассировкой происходящего или нет).
		auto actual_handler = [&ioctx, &generator](auto req, auto /*params*/) {
				return handler(ioctx, generator, std::move(req));
			};

		// Если должна использоваться трассировка запросов, то должен
		// запускаться один тип сервера.
		if(cfg.config_.tracing_) {
			run_server<traceable_server_traits_t>(
					ioctx, cfg.config_, std::move(actual_handler));
		}
		else {
			// Трассировка не нужна, запускается другой тип сервера.
			run_server<non_traceable_server_traits_t>(
					ioctx, cfg.config_, std::move(actual_handler));
		}

		// Все, теперь ждем завершения работы сервера.
	}
	catch( const std::exception & ex ) {
		std::cerr << "Error: " << ex.what() << std::endl;
		return 2;
	}

	return 0;
}

