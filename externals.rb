MxxRu::git_externals :asio do |e|
  e.url 'https://github.com/chriskohlhoff/asio.git'
  e.commit '9229964dc1853b375077338cb398027a6dbbb148' # Dec 10 2017

  e.map_dir 'asio/include' => 'dev/asio'
end

MxxRu::arch_externals :asio_mxxru do |e|
  e.url 'https://bitbucket.org/sobjectizerteam/asio_mxxru-1.1/get/1.1.2.tar.bz2'

  e.map_dir 'dev/asio_mxxru' => 'dev'
end

MxxRu::arch_externals :nodejs_http_parser do |e|
  e.url 'https://github.com/nodejs/http-parser/archive/v2.7.1.tar.gz'

  e.map_file 'http_parser.h' => 'dev/nodejs/http_parser/*'
  e.map_file 'http_parser.c' => 'dev/nodejs/http_parser/*'
end

MxxRu::arch_externals :nodejs_http_parser_mxxru do |e|
  e.url 'https://bitbucket.org/sobjectizerteam/nodejs_http_parser_mxxru-0.1/get/v.0.1.0.tar.bz2'

  e.map_dir 'dev/nodejs/http_parser_mxxru' => 'dev/nodejs'
end

MxxRu::arch_externals :fmt do |e|
  e.url 'https://github.com/fmtlib/fmt/archive/4.1.0.zip'

  e.map_dir 'fmt' => 'dev/fmt'
end

MxxRu::arch_externals :fmtlib_mxxru do |e|
  e.url 'https://bitbucket.org/sobjectizerteam/fmtlib_mxxru-0.1/get/v.0.1.0.tar.bz2'

  e.map_dir 'dev/fmt_mxxru' => 'dev'
end

MxxRu::arch_externals :clara do |e|
  e.url 'https://github.com/catchorg/Clara/archive/v1.1.2.tar.gz'

  e.map_file 'single_include/clara.hpp' => 'dev/clara/*'
end

MxxRu::arch_externals :restinio do |e|
  e.url 'https://bitbucket.org/sobjectizerteam/restinio-0.4/get/v.0.4.2.tar.bz2'

  e.map_dir 'dev/restinio' => 'dev'

  # For building nodejs http-parser with cmake.
  e.map_file 'dev/nodejs/http_parser/CMakeLists.txt' => 'dev/nodejs/http_parser/*'
end

MxxRu::arch_externals :cpp_util do |e|
  e.url 'https://bitbucket.org/sobjectizerteam/cpp_util-3.0/get/v.3.0.0-rc8.tar.bz2'

  e.map_dir 'dev/cpp_util_3' => 'dev'
end

