#!/usr/bin/ruby
require 'mxx_ru/cpp'

MxxRu::Cpp::composite_target( MxxRu::BUILD_ROOT ) {

	toolset.force_cpp14
	global_include_path "."
	global_include_path "clara"

	if 'gcc' == toolset.name || 'clang' == toolset.name
		global_linker_option '-pthread'
		global_linker_option '-static-libstdc++'
		global_linker_option "-Wl,-rpath='$ORIGIN'"

		if 'unix' == toolset.tag('target_os', 'UNKNOWN') &&
				'freebsd' == toolset.tag('unix_port', 'UNKNOWN')
			global_compiler_option '-I/usr/local/include'
			global_linker_option '-L/usr/local/lib'
		end
	end

	# If there is local options file then use it.
	if FileTest.exist?( "local-build.rb" )
		required_prj "local-build.rb"
	else
		default_runtime_mode( MxxRu::Cpp::RUNTIME_RELEASE )
		MxxRu::enable_show_brief

		global_obj_placement MxxRu::Cpp::PrjAwareRuntimeSubdirObjPlacement.new(
			'target', MxxRu::Cpp::PrjAwareRuntimeSubdirObjPlacement::USE_COMPILER_ID )
	end

	required_prj 'delay_server/prj.rb'
	required_prj 'bridge_server_1/prj.rb'
	required_prj 'bridge_server_1_pipe/prj.rb'
	required_prj 'bridge_server_2/prj.rb'
}

