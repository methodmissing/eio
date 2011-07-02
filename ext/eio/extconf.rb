require 'mkmf'
dir_config('eio')
$defs << "-DRUBY_VM" if have_func('rb_thread_blocking_region')
libs_path = File.expand_path(File.join(File.dirname(__FILE__), '../libeio/.libs'))
find_library("eio", "eio_init", libs_path)
$defs << "-pedantic"
# leeched from ext/openssl
unless try_compile("#define FOO(...) foo(__VA_ARGS__)\n int x(){FOO(1);FOO(1,2);FOO(1,2,3);}\n")
  abort("Your environment do not support the __VA_ARGS__ macro")
end
create_makefile('eio_ext')