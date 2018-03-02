require 'mxx_ru/cpp'
require 'restinio/asio_helper.rb'

MxxRu::Cpp::exe_target {

  target 'bridge_server_2'

  RestinioAsioHelper.attach_propper_asio( self )
  required_prj 'nodejs/http_parser_mxxru/prj.rb'
  required_prj 'fmt_mxxru/prj.rb'
  required_prj 'restinio/platform_specific_libs.rb'

  lib 'curl'

  cpp_source 'main.cpp'
}
