Gem::Specification.new do |s|
  s.name = "eio"
  s.version = defined?(EIO) ? EIO::VERSION : (ENV['VERSION'] || '0.1')
  s.summary = "eio - a libeio wrapper for Ruby"
  s.description = "eio - a libeio wrapper for Ruby"
  s.authors = ["Lourens Naudé"]
  s.email = ["lourens@methodmissing.com"]
  s.homepage = "http://github.com/methodmissing/eio"
  s.date = Time.now.utc.strftime('%Y-%m-%d')
  s.platform = Gem::Platform::RUBY
  s.extensions = FileList["ext/eio/extconf.rb"]
  s.files = `git ls-files`.split
  s.test_files = Dir['test/test_*.rb']
  s.add_development_dependency('rake-compiler', '~> 0.7.7')
  s.add_development_dependency('eventmachine', '~> 0.12.3')
end