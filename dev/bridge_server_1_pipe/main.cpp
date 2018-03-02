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

//
// ПРИМЕЧАНИЕ: ДЛЯ ПРОСТОТЫ И КОМПАКТНОСТИ РЕАЛИЗАЦИИ КОДЫ ВОЗВРАТА
// ВЫЗЫВАЕМЫХ ИЗ libcurl ФУНКЦИЙ НЕ ПРОВЕРЯЮТСЯ.
// ТАК ЖЕ НЕ ПРОВЕРЯЮТСЯ КОДЫ ВОЗВРАТА СИСТЕМНЫХ ФУНКЦИЙ ВРОДЕ
// pipe, read, write И Т.Д.
//

// Вспомогательная штука, чтобы подавить предупреждения об игнорировании
// возвращаемого значения.
namespace {
	struct just_ignore_t {
		template<typename T> void operator=(T) {}
	} _;
}

// Примитивная реализация thread-safe контейнера для обмена информацией
// между разными рабочими нитями.
// Позволяет только поместить новый элемент в контейнер и попробовать взять
// элемент из контейнера. Никакого ожидания на попытке извлечения элемента
// из пустого контейнера нет.
//
// Для того, чтобы читающая сторона могла определить момент, когда контейнер
// перестал быть пустым, используется нотификационные каналы. Создается
// обычный Unix-овый пайп (посредством pipe()). Когда в пустой контейнер
// помещается новое значение, производится запись в write-end этого пайпа.
// Соответственно, ждущая сторона обнаружит, что read-end этого пайпа
// готов к чтению. После чего ждущая сторона вызовет метод pop() для извлечения
// содержимого контейнера.
// Доступ к read-end нотификационного пайпа можно получить посредством метода
// read_pipefd().
//
template<typename T>
class thread_safe_queue_t {
	using unique_ptr_t = std::unique_ptr<T>;

	std::mutex lock_;
	std::queue<unique_ptr_t> content_;

	bool closed_{false};

	int pipefd_[2];

	auto write_pipefd() const noexcept { return pipefd_[1]; }

	void notify_if_necessary(bool was_empty) {
		if(was_empty) {
			int dummy{0};
			_ = ::write(write_pipefd(), &dummy, sizeof(dummy));
		}
	}

public:
	enum class status_t {
		extracted,
		empty_queue,
		closed
	};

	thread_safe_queue_t() {
		// Создаем нотификационный пайп.
		_ = ::pipe(pipefd_);

		// Переводим пайп в неблокирующий режим.
		_ = ::fcntl(write_pipefd(), F_SETFL, O_DIRECT | O_NONBLOCK);
		_ = ::fcntl(read_pipefd(), F_SETFL, O_DIRECT | O_NONBLOCK);
	}
	~thread_safe_queue_t() {
		// Нотификационный пайп должен быть закрыт вручную.
		::close(pipefd_[0]);
		::close(pipefd_[1]);
	}

	auto read_pipefd() const noexcept { return pipefd_[0]; }

	void push(unique_ptr_t what) {
		std::lock_guard<std::mutex> l{lock_};

		bool was_empty = content_.empty();
		content_.emplace(std::move(what));

		notify_if_necessary(was_empty);
	}

	// Метод pop получает лямбда-функцию, в которую будут поочередно
	// переданы все элементы из контейнера, если контейнер не пуст.
	// Передача будет осуществляться при захваченном mutex-е, что означает,
	// что новые элементы не могут быть помещенны в очередь, пока pop()
	// не завершит свою работу.
	template<typename Acceptor>
	status_t pop(Acceptor && acceptor) {
		// Сперва вычитаем значение из нотификационного канала, которое
		// там должно быть.
		{
			int dummy{0};
			_ = ::read(read_pipefd(), &dummy, sizeof(dummy));
		}
		
		// Вот теперь можно захватывать mutex и очищать содержимое контейнера.
		std::lock_guard<std::mutex> l{lock_};
		if(closed_) {
			return status_t::closed;
		}
		else if(content_.empty()) {
			return status_t::empty_queue;
		}
		else {
			while(!content_.empty()) {
				acceptor(std::move(content_.front()));
				content_.pop();
			}
			return status_t::extracted;
		}
	}

	void close() {
		std::lock_guard<std::mutex> l{lock_};
		closed_ = true;

		notify_if_necessary(content_.empty());
	}
};

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

// Тип контейнера для обмена информацией между рабочими нитями.
using request_info_queue_t = thread_safe_queue_t<request_info_t>;

// Эту функцию будет вызывать curl когда начнут приходить данные
// от удаленного сервера. Указатель на нее будет задан через
// CURLOPT_WRITEFUNCTION.
std::size_t write_callback(
		char *ptr,
		size_t size,
		size_t nmemb,
		void *userdata) {
	auto info = reinterpret_cast<request_info_t *>(userdata);
	const auto total_size = size * nmemb;
	info->reply_data_.append(ptr, total_size);

	return total_size;
}

// Создать curl_easy для нового исходящего запроса, заполнить все нужные
// для него значения и передать этот новый curl_easy в curl_multi.
void introduce_new_request_to_curl_multi(
		CURLM * curlm,
		std::unique_ptr<request_info_t> info) {
	// Создаем и подготавливаем curl_easy экземпляр для нового запроса.
	CURL * h = curl_easy_init();
	curl_easy_setopt(h, CURLOPT_URL, info->url_.c_str());
	curl_easy_setopt(h, CURLOPT_PRIVATE, info.get());

	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, info.get());

	// Новый curl_easy подготовлен, можно отдать его в curl_multi.
	curl_multi_add_handle(curlm, h);

	// unique_ptr не должен больше нести ответственность за объект.
	// Мы его сами удалим когда обработка запроса завершится.
	info.release();
}

// Попытка извлечения всех запросов, которые ждут в очереди.
// Если возвращается status_t::closed, значит работа должна быть
// остановлена.
auto try_extract_new_requests(request_info_queue_t & queue, CURLM * curlm) {
	return queue.pop([curlm](auto info) {
			introduce_new_request_to_curl_multi(curlm, std::move(info));
		});
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

// Реализация рабочей нити, на которой будут выполняться операции
// curl_multi_perform.
void curl_multi_work_thread(request_info_queue_t & queue) {
	using namespace cpp_util_3;

	// Инциализируем сам curl.
	curl_global_init(CURL_GLOBAL_ALL);
	auto curl_global_deinitializer =
			at_scope_exit([]{ curl_global_cleanup(); });

	// Создаем экземпляр curl_multi, который нам потребуется для выполнения
	// запросов к удаленному серверу.
	auto curlm = curl_multi_init();
	auto curlm_destroyer = at_scope_exit([&]{ curl_multi_cleanup(curlm); });

	// Сколько сейчас запросов находится в обработке.
	int still_running{0};

	while(true) {
		curl_waitfd notify_fd;
		notify_fd.fd = queue.read_pipefd();
		notify_fd.events = CURL_WAIT_POLLIN;
		notify_fd.revents = 0;

		int numfds{0};
		curl_multi_wait(curlm, &notify_fd, 1, 5000, &numfds);

		if(numfds && 0 != notify_fd.revents) {
			// Нужно забирать новые заявки.
			auto status = try_extract_new_requests(queue, curlm);
			if(request_info_queue_t::status_t::closed == status)
				// Работу нужно завершать.
				// Запросы, которые остались необработанными оставляем как есть.
				return;
		}

		// Если удалось что-то извлечь или если есть незавершенные операции,
		// то вызываем curl_multi_perform.
		if(still_running || numfds) {
			curl_multi_perform(curlm, &still_running);
			// Пытаемся проверить, закончились ли какие-нибудь операции.
			check_curl_op_completion(curlm);
		}
	}
}

// Реализация обработчика запросов.
restinio::request_handling_status_t handler(
		const config_t & config,
		request_info_queue_t & queue,
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

		queue.push(std::move(info));

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
		Handler && handler) {
	restinio::run(
			restinio::on_this_thread<Server_Traits>()
				.address(config.address_)
				.port(config.port_)
				.request_handler(std::forward<Handler>(handler)));
}

int main(int argc, char ** argv) {
	try {
		const auto cfg = parse_cmd_line_args(argc, argv);
		if(cfg.help_requested_)
			return 1;

		// Нам потребуется контейнер для передачи информации между
		// рабочими нитями.
		request_info_queue_t queue;

		// Актуальный обработчик входящих HTTP-запросов.
		auto actual_handler = [&cfg, &queue](auto req) {
				return handler(cfg.config_, queue, std::move(req));
			};

		// Запускаем отдельную рабочую нить, на которой будут выполняться
		// запросы к удаленному серверу посредством curl_multi_perform.
		std::thread curl_thread{[&queue]{ curl_multi_work_thread(queue); }};
		// Защищаемся от выхода из скоупа без предварительного останова
		// этой отдельной рабочей нити.
		auto curl_thread_stopper = cpp_util_3::at_scope_exit([&] {
				queue.close();
				curl_thread.join();
			});

		// Теперь можно запустить основной HTTP-сервер.

		// Если должна использоваться трассировка запросов, то должен
		// запускаться один тип сервера.
		if(cfg.config_.tracing_) {
			// Для того, чтобы сервер трассировал запросы, нужно определить
			// свой класс свойств для сервера.
			struct traceable_server_traits_t : public restinio::default_single_thread_traits_t {
				// Определяем нужный нам тип логгера.
				using logger_t = restinio::single_threaded_ostream_logger_t;
			};
			// Теперь используем этот новый класс свойств для запуска сервера.
			run_server<traceable_server_traits_t>(
					cfg.config_, std::move(actual_handler));
		}
		else {
			// Трассировка не нужна, поэтому запускаем обычный штатный сервер.
			run_server<restinio::default_single_thread_traits_t>(
					cfg.config_, std::move(actual_handler));
		}

		// Все, теперь ждем завершения работы сервера.
	}
	catch( const std::exception & ex ) {
		std::cerr << "Error: " << ex.what() << std::endl;
		return 2;
	}

	return 0;
}

