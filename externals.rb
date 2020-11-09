MxxRu::arch_externals :asio do |e|
  e.url 'https://github.com/chriskohlhoff/asio/archive/asio-1-16-1.tar.gz'

  e.map_dir 'asio/include' => 'dev/asio'
end

MxxRu::arch_externals :asio_mxxru do |e|
  e.url 'https://github.com/Stiffstream/asio_mxxru/archive/1.1.2.tar.gz'

  e.map_dir 'dev/asio_mxxru' => 'dev'
end

MxxRu::arch_externals :nodejs_http_parser do |e|
  e.url 'https://github.com/nodejs/http-parser/archive/v2.9.4.tar.gz'

  e.map_file 'http_parser.h' => 'dev/nodejs/http_parser/*'
  e.map_file 'http_parser.c' => 'dev/nodejs/http_parser/*'
end

MxxRu::arch_externals :nodejs_http_parser_mxxru do |e|
  e.url 'https://github.com/Stiffstream/nodejs_http_parser_mxxru/archive/v.0.2.1.tar.gz'

  e.map_dir 'dev/nodejs/http_parser_mxxru' => 'dev/nodejs'
end

MxxRu::arch_externals :fmt do |e|
  e.url 'https://github.com/fmtlib/fmt/archive/4.1.0.zip'

  e.map_dir 'fmt' => 'dev/fmt'
end

MxxRu::arch_externals :fmtlib_mxxru do |e|
  e.url 'https://github.com/Stiffstream/fmtlib_mxxru/archive/fmt-4.1.0.tar.gz'

  e.map_dir 'dev/fmt_mxxru' => 'dev'
end

MxxRu::arch_externals :clara do |e|
  e.url 'https://github.com/catchorg/Clara/archive/v1.1.2.tar.gz'

  e.map_file 'single_include/clara.hpp' => 'dev/clara/*'
end

MxxRu::arch_externals :restinio do |e|
  e.url 'https://github.com/Stiffstream/restinio/archive/v.0.4.9.1.tar.gz'

  e.map_dir 'dev/restinio' => 'dev'

  # For building nodejs http-parser with cmake.
  e.map_file 'dev/nodejs/http_parser/CMakeLists.txt' => 'dev/nodejs/http_parser/*'
end

MxxRu::arch_externals :cpp_util do |e|
  e.url 'https://github.com/Stiffstream/cpp-util/archive/c15d41e16d611ca45d2741c486f12df848eebcae.tar.gz'

  e.map_dir 'dev/cpp_util_3' => 'dev'
end

