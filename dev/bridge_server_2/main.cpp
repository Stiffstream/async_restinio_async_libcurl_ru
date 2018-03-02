#include <iostream>
#include <queue>

#include <restinio/all.hpp>

#include <clara.hpp>

#include <fmt/format.h>

#include <cpp_util_3/at_scope_exit.hpp>

#include <curl/curl.h>

// Конфигурация, которая потребуется серверу.
struct config_t {
	// Адрес, на котором нужно слушать новые входящие запросы.
	std::string address_{"localhost"};
	// Порт, на котором нужно слушать.
	std::uint16_t port_{8080};

	// Адрес, на который нужно адресовать собственные запросы.
	std::string target_address_{"localhost"};
	// Порт, на который нужно адресовать собственные запросы.
	std::uint16_t target_port_{8090};

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

	// Подготавливаем парсер аргументов командной строки.
	using namespace clara;

	auto cli = Opt(result.config_.address_, "address")["-a"]["--address"]
				("address to listen (default: localhost)")
		| Opt(result.config_.port_, "port")["-p"]["--port"]
				(fmt::format("port to listen (default: {})", result.config_.port_))
		| Opt(result.config_.target_address_, "target address")["-T"]["--target-address"]
				(fmt::format("target address (default: {})", result.config_.target_address_))
		| Opt(result.config_.target_port_, "target port")["-P"]["--target-port"]
				(fmt::format("target port (default: {})", result.config_.target_port_))
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

	return result;
}

// Сообщение, которое будет передаваться на рабочую нить с curl_multi_perform
// для того, чтобы выполнить запрос к удаленному серверу.
struct request_info_t {
	// URL, на который нужно выполнить обращение.
	const std::string url_;

	// Запрос, в рамках которого нужно сделать обращение к удаленному серверу.
	restinio::request_handle_t original_req_;

	// Код ошибки от самого curl-а.
	CURLcode curl_code_{CURLE_OK};

	// Код ответа удаленного сервера.
	// Имеет актуальное значение только если сервер ответил.
	long response_code_{0};

	// Ответные данные, которые будут получены от удаленного сервера.
	std::string reply_data_;

	request_info_t(std::string url, restinio::request_handle_t req)
		:	url_{std::move(url)}, original_req_{std::move(req)}
		{}
};

//
// ПРИМЕЧАНИЕ: ДЛЯ ПРОСТОТЫ И КОМПАКТНОСТИ РЕАЛИЗАЦИИ КОДЫ ВОЗВРАТА
// ВЫЗЫВАЕМЫХ ИЗ libcurl ФУНКЦИЙ НЕ ПРОВЕРЯЮТСЯ.
//

// Эту функцию будет вызывать curl когда начнут приходить данные
// от удаленного сервера. Указатель на нее будет задан через
// CURLOPT_WRITEFUNCTION.
std::size_t write_callback(
		char *ptr, size_t size, size_t nmemb, void *userdata) {
	auto info = reinterpret_cast<request_info_t *>(userdata);
	const auto total_size = size * nmemb;
	info->reply_data_.append(ptr, total_size);

	return total_size;
}

// Финальная стадия обработки запроса к удаленному серверу.
// curl_multi свою часть работы сделал. Осталось создать http-response,
// который будет отослан в ответ на входящий http-request.
void complete_request_processing(request_info_t & info) {
	auto response = info.original_req_->create_response();

	response.append_header(restinio::http_field::server,
			"RESTinio hello world server");
	response.append_header_date_field();
	response.append_header(restinio::http_field::content_type,
			"text/plain; charset=utf-8");

	if(CURLE_OK == info.curl_code_) {
		if(200 == info.response_code_)
			response.set_body(
				fmt::format("Request processed.\nPath: {}\nQuery: {}\n"
						"Response:\n===\n{}\n===\n",
					info.original_req_->header().path(),
					info.original_req_->header().query(),
					info.reply_data_));
		else
			response.set_body(
				fmt::format("Request failed.\nPath: {}\nQuery: {}\n"
						"Response code: {}\n",
					info.original_req_->header().path(),
					info.original_req_->header().query(),
					info.response_code_));
	}
	else
		response.set_body("Target service unavailable\n");

	response.done();
}

// Попытка обработать все сообщения, которые на данный момент существуют
// в curl_multi.
void check_curl_op_completion(CURLM * curlm) {
	CURLMsg * msg;
	int messages_left{0};

	// В цикле извлекаем все сообщения от curl_multi и обрабатываем
	// только сообщения CURLMSG_DONE.
	while(nullptr != (msg = curl_multi_info_read(curlm, &messages_left))) {
		if(CURLMSG_DONE == msg->msg) {
			// Нашли операцию, которая реально завершилась.
			// Сразу забераем ее под unique_ptr, дабы не забыть вызвать
			// curl_easy_cleanup.
			std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy_handle{
					msg->easy_handle,
					&curl_easy_cleanup};

			// Эта операция в curl_multi больше участвовать не должна.
			curl_multi_remove_handle(curlm, easy_handle.get());

			// Разбираемся с оригинальным запросом, с которым эта операция
			// была связана.
			request_info_t * info_raw_ptr{nullptr};
			curl_easy_getinfo(easy_handle.get(), CURLINFO_PRIVATE, &info_raw_ptr);
			// Сразу оборачиваем в unique_ptr, чтобы удалить объект.
			std::unique_ptr<request_info_t> info{info_raw_ptr};

			info->curl_code_ = msg->data.result;
			if(CURLE_OK == info->curl_code_) {
				// Нужно достать код, с которым нам ответил сервер.
				curl_easy_getinfo(
						easy_handle.get(),
						CURLINFO_RESPONSE_CODE,
						&info->response_code_);
			}

			// Теперь уже можно завершить обработку.
			complete_request_processing(*info);
		}
	}
}

// Вспомогательный класс для работы с сокетом.
class active_socket_t final
{
public:
	using status_t = std::int_fast8_t;

	static constexpr status_t poll_in = 1u;
	static constexpr status_t poll_out = 2u;

private:
	restinio::asio_ns::ip::tcp::socket socket_;
	status_t status_{0};

public:
	active_socket_t(restinio::asio_ns::io_service & io_service)
		:	socket_{io_service, restinio::asio_ns::ip::tcp::v4()}
		{}

	auto & socket() noexcept { return socket_; }

	auto handle() noexcept { return socket_.native_handle(); }

	void clear_status() noexcept { status_ = 0; }

	auto status() noexcept { return status_; }

	void update_status( status_t flag ) noexcept { status_ |= flag; }
};

// Реализация работы с curl_multi через curl_multi_socket_action.
class curl_multi_processor_t {
public:
	curl_multi_processor_t(restinio::asio_ns::io_context & ioctx);
	~curl_multi_processor_t();

	// Это не Copyable и не Moveable класс.
	curl_multi_processor_t(const curl_multi_processor_t &) = delete;
	curl_multi_processor_t(curl_multi_processor_t &&) = delete;

	// Единственная публичная функция, которую будут вызывать для
	// того, чтобы выполнить очередной запрос к удаленному серверу.
	void perform_request(std::unique_ptr<request_info_t> info);

private:
	// Экземпляр curl_multi, который будет выполнять работу с исходящими запросами.
	CURLM * curlm_;

	// Asio-шный контекст, на котором будет идти работа.
	restinio::asio_ns::io_context & ioctx_;
	// Защита от одновременной диспетчеризации сразу на нескольких нитях.
	restinio::asio_ns::strand<restinio::asio_ns::executor> strand_{ioctx_.get_executor()};

	// Таймер, который будем использовать внутри timer_function-коллбэка.
	restinio::asio_ns::steady_timer timer_{ioctx_};

	// Множество еще живых сокетов, созданных для обслуживания запросов
	// к удаленному серверу.
	std::unordered_map<curl_socket_t, std::unique_ptr<active_socket_t>> active_sockets_;

	// Вспомогательная функция, чтобы не выписывать reinterpret_cast вручную.
	static auto cast_to(void * ptr) {
		return reinterpret_cast<curl_multi_processor_t *>(ptr);
	}

	// Коллбэк для CURLMOPT_SOCKETFUNCTION.
	static int socket_function(
			CURL *,
			curl_socket_t s,
			int what,
			void * userp, void *);

	// Коллбэк для CURLMOPT_TIMERFUNCTION.
	static int timer_function(CURLM *, long timeout_ms, void * userp);
	// Вспомогательная функция для проверки истечения таймаутов.
	void check_timeouts();

	// Вспомогательная функция, которая будет вызываться, когда какой-либо
	// из сокетов готов к чтению или записи.
	void event_cb(
			curl_socket_t socket,
			int what,
			const restinio::asio_ns::error_code & ec);

	// Коллбэк для CURLOPT_OPENSOCKETFUNCTION.
	static curl_socket_t open_socket_function(
			void * cbp,
			curlsocktype type,
			curl_sockaddr * addr);

	// Коллбэк для CURLOPT_CLOSESOCKETFUNCTION.
	static int close_socket_function(void * cbp, curl_socket_t socket);

	// Вспомогательные функции для того, чтобы заставить Asio отслеживать
	// готовность сокета к операциям чтения и записи.
	void schedule_wait_read_for(active_socket_t & act_socket);
	void schedule_wait_write_for(active_socket_t & act_socket);
};

curl_multi_processor_t::curl_multi_processor_t(
		restinio::asio_ns::io_context & ioctx)
	:	curlm_{curl_multi_init()}
	,	ioctx_{ioctx} {

	// Должным образом настраиваем curl_multi.
	
	// Коллбэк для обработки связанных с сокетом операций.
	curl_multi_setopt(curlm_, CURLMOPT_SOCKETFUNCTION,
		&curl_multi_processor_t::socket_function);
	curl_multi_setopt(curlm_, CURLMOPT_SOCKETDATA, this);

	// Коллбэк для обработки связанных с таймером операций.
	curl_multi_setopt(curlm_, CURLMOPT_TIMERFUNCTION,
		&curl_multi_processor_t::timer_function);
	curl_multi_setopt(curlm_, CURLMOPT_TIMERDATA, this);
}

curl_multi_processor_t::~curl_multi_processor_t() {
	curl_multi_cleanup(curlm_);
}

void curl_multi_processor_t::perform_request(
		std::unique_ptr<request_info_t> info) {
	// Для того, чтобы передать новый запрос в curl_multi используем
	// callback для Asio.
	restinio::asio_ns::post(strand_,
		[this, info = std::move(info)]() mutable {
			// Для выполнения очередного запроса нужно создать curl_easy-объект и
			// должным образом его настроить.
			auto handle = curl_easy_init();

			// Обычные для curl_easy настройки, вроде URL и writefunction.
			curl_easy_setopt(handle, CURLOPT_URL, info->url_.c_str());
			curl_easy_setopt(handle, CURLOPT_PRIVATE, info.get());

			curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
			curl_easy_setopt(handle, CURLOPT_WRITEDATA, info.get());
			// Не совсем обычные настройки.
			// Здесь мы определяем, как будет создаваться новый сокет для
			// обработки запроса.
			curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION,
				&curl_multi_processor_t::open_socket_function);
			curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA, this);

			// А здесь определяем, как ставший ненужным сокет будет закрываться.
			curl_easy_setopt(handle, CURLOPT_CLOSESOCKETFUNCTION,
				&curl_multi_processor_t::close_socket_function);
			curl_easy_setopt(handle, CURLOPT_CLOSESOCKETDATA, this);

			// Новый curl_easy подготовлен, можно отдать его в curl_multi.
			curl_multi_add_handle(curlm_, handle);

			// unique_ptr не должен больше нести ответственность за объект.
			// Мы его сами удалим когда обработка запроса завершится.
			info.release();
		});
}

int curl_multi_processor_t::socket_function(
		CURL *,
		curl_socket_t s,
		int what,
		void * userp, void *) {
	auto self = cast_to(userp);
	// Сокет, над которым нужно выполнить действие, должен быть среди живых.
	// Если его там нет, то просто игнорируем операцию.
	const auto it = self->active_sockets_.find(s);
	if(it != self->active_sockets_.end()) {
		auto & act_socket = *(it->second);

		// Сбрасываем текущий статус для сокета. Новый статус будет выставлен
		// на основании значения флага what.
		act_socket.clear_status();

		// Определяем новый статус для сокета.
		if(CURL_POLL_IN == what || CURL_POLL_INOUT == what) {
			// Требуется проверка готовности к чтению данных.
			act_socket.update_status(active_socket_t::poll_in);
			self->schedule_wait_read_for(act_socket);
		}
		if(CURL_POLL_OUT == what || CURL_POLL_INOUT == what) {
			// Требуется проверка готовности к записи данных.
			act_socket.update_status(active_socket_t::poll_out);
			self->schedule_wait_write_for(act_socket);
		}
	}

	return 0;
}

int curl_multi_processor_t::timer_function(
		CURLM *,
		long timeout_ms,
		void * userp) {
	auto self = cast_to(userp);

	if(timeout_ms < 0) {
		// Старый таймер удаляем.
		self->timer_.cancel();
	}
	else if(0 == timeout_ms) {
		// Сразу же проверяем истечение тайм-аутов для активных операций.
		self->check_timeouts();
	}
	else {
		// Нужно взводить новый таймер.
		self->timer_.cancel();
		self->timer_.expires_after(std::chrono::milliseconds{timeout_ms});
		self->timer_.async_wait(
				restinio::asio_ns::bind_executor(self->strand_,
					[self](const auto & ec) {
						if( !ec )
							self->check_timeouts();
					}));
	}

	return 0;
}

void curl_multi_processor_t::check_timeouts() {
	int running_handles_count = 0;
	// Заставляем curl проверить состояние активных операций.
	curl_multi_socket_action(curlm_, CURL_SOCKET_TIMEOUT, 0, &running_handles_count);
	// После чего проверяем завершилось ли что-нибудь.
	check_curl_op_completion(curlm_);
}

void curl_multi_processor_t::event_cb(
		curl_socket_t socket,
		int what,
		const restinio::asio_ns::error_code & ec) {
	// Прежде всего нужно найти сокет среди живых. Может быть, что его
	// там уже нет. В этом случае ничего делать не нужно.
	auto it = active_sockets_.find(socket);
	if(it != active_sockets_.end()) {
		if( ec )
			what = CURL_CSELECT_ERR;

		int running_handles_count = 0;
		// Заставляем curl проверить состояние этого сокета.
		curl_multi_socket_action(curlm_, socket, what, &running_handles_count );
		// После чего проверяем завершилось ли что-нибудь.
		check_curl_op_completion(curlm_);

		if(running_handles_count <= 0)
			// Больше нет активных операций. Таймер уже не нужен.
			timer_.cancel();

		// Еще раз ищем сокет среди живых, т.к. он мог исчезнуть внутри
		// вызовов curl_multi_socket_action и check_active_sockets.
		it = active_sockets_.find(socket);
		if(!ec && it != active_sockets_.end()) {
			// Сокет все еще жив и подлежит обработке.
			auto & act_socket = *(it->second);

			// Проверяем, в каких операциях сокет нуждается и инициируем
			// эти операции.
			if(CURL_POLL_IN == what &&
					0 != (active_socket_t::poll_in & act_socket.status())) {
				schedule_wait_read_for(act_socket);
			}
			if(CURL_POLL_OUT == what &&
					0 != (active_socket_t::poll_out & act_socket.status())) {
				schedule_wait_write_for(act_socket);
			}
		}
	}
}

curl_socket_t curl_multi_processor_t::open_socket_function(
		void * cbp,
		curlsocktype type,
		curl_sockaddr * addr) {
	auto self = cast_to(cbp);
	curl_socket_t sockfd = CURL_SOCKET_BAD;

	// В данном примере ограничиваем себя только IPv4.
	if(CURLSOCKTYPE_IPCXN == type && AF_INET == addr->family) {
		// Создаем сокет, который затем будет использоваться для взаимодействия
		// с удаленным сервером.
		auto act_socket = std::make_unique<active_socket_t>(self->ioctx_);
		const auto native_handle = act_socket->handle();
		
		// Новый сокет должен быть сохранен во множестве живых сокетов.
		self->active_sockets_.emplace( native_handle, std::move(act_socket) );
		
		sockfd = native_handle;
	}

	return sockfd;
}

int curl_multi_processor_t::close_socket_function(
		void * cbp,
		curl_socket_t socket) {
	auto self = cast_to(cbp);
	// Достаточно просто изъять сокет из множества живых сокетов.
	// Закрытие произойдет автоматически в деструкторе active_socket_t.
	self->active_sockets_.erase(socket);

	return 0;
}

void curl_multi_processor_t::schedule_wait_read_for(
		active_socket_t & act_socket) {
	act_socket.socket().async_wait(
		restinio::asio_ns::ip::tcp::socket::wait_read,
		restinio::asio_ns::bind_executor(strand_,
			[this, s = act_socket.handle()]( const auto & ec ){
				this->event_cb(s, CURL_POLL_IN, ec);
			}));
}

void curl_multi_processor_t::schedule_wait_write_for(
		active_socket_t & act_socket) {
	act_socket.socket().async_wait(
		restinio::asio_ns::ip::tcp::socket::wait_write,
		restinio::asio_ns::bind_executor(strand_,
			[this, s = act_socket.handle()]( const auto & ec ){
				this->event_cb(s, CURL_POLL_OUT, ec);
			}));
}

// Реализация обработчика запросов.
restinio::request_handling_status_t handler(
		const config_t & config,
		curl_multi_processor_t & req_processor,
		restinio::request_handle_t req) {
	if(restinio::http_method_get() == req->header().method()
			&& "/data" == req->header().path()) {
		// Разберем дополнительные параметры запроса.
		const auto qp = restinio::parse_query(req->header().query());

		// Нужно оформить объект с информацией о запросе и передать
		// его на обработку в нить curl_multi.
		auto url = fmt::format("http://{}:{}/{}/{}/{}",
				config.target_address_,
				config.target_port_,
				qp["year"], qp["month"], qp["day"]);

		auto info = std::make_unique<request_info_t>(
				std::move(url), std::move(req));

		req_processor.perform_request(std::move(info));

		// Подтверждаем, что мы приняли запрос к обработке и что когда-то
		// мы ответ сгенерируем.
		return restinio::request_accepted();
	}

	// Все остальные запросы нашим демонстрационным сервером отвергаются.
	return restinio::request_rejected();
}

// Вспомогательная функция, которая отвечает за запуск сервера нужного типа.
template<typename Server_Traits, typename Handler>
void run_server(
		const config_t & config,
		restinio::asio_ns::io_context & ioctx,
		Handler && handler) {
	restinio::run(
			ioctx,
			restinio::on_thread_pool<Server_Traits>(std::thread::hardware_concurrency())
				.address(config.address_)
				.port(config.port_)
				.request_handler(std::forward<Handler>(handler)));
}

int main(int argc, char ** argv) {
	try {
		const auto cfg = parse_cmd_line_args(argc, argv);
		if(cfg.help_requested_)
			return 1;

		// Инциализируем сам curl.
		curl_global_init(CURL_GLOBAL_ALL);
		auto curl_global_deinitializer =
				cpp_util_3::at_scope_exit([]{ curl_global_cleanup(); });

		// Сами создаем Asio-шный io_context, т.к. он будет использоваться
		// и curl_multi_processor-ом, и нашим HTTP-сервером.
		restinio::asio_ns::io_context ioctx;

		// Обработчик запросов к удаленному серверу.
		curl_multi_processor_t curl_multi{ioctx};

		// Актуальный обработчик входящих HTTP-запросов.
		auto actual_handler = [&cfg, &curl_multi](auto req) {
				return handler(cfg.config_, curl_multi, std::move(req));
			};

		// Теперь можно запустить основной HTTP-сервер.

		// Если должна использоваться трассировка запросов, то должен
		// запускаться один тип сервера.
		if(cfg.config_.tracing_) {
			// Для того, чтобы сервер трассировал запросы, нужно определить
			// свой класс свойств для сервера.
			struct traceable_server_traits_t : public restinio::default_traits_t {
				// Определяем нужный нам тип логгера.
				using logger_t = restinio::shared_ostream_logger_t;
			};
			// Теперь используем этот новый класс свойств для запуска сервера.
			run_server<traceable_server_traits_t>(
					cfg.config_, ioctx, std::move(actual_handler));
		}
		else {
			// Трассировка не нужна, поэтому запускаем обычный штатный сервер.
			run_server<restinio::default_traits_t>(
					cfg.config_, ioctx, std::move(actual_handler));
		}

		// Все, теперь ждем завершения работы сервера.
	}
	catch( const std::exception & ex ) {
		std::cerr << "Error: " << ex.what() << std::endl;
		return 2;
	}

	return 0;
}

