# encoding: utf-8

require 'rake/extensiontask'
require 'rake/testtask'
require 'rdoc/task'

spec = eval(IO.read('eio.gemspec'))

task :compile => :build_libeio
task :clobber => :clobber_libeio

Rake::ExtensionTask.new('eio', spec) do |ext|
  ext.name = 'eio_ext'
  ext.ext_dir = 'ext/eio'
end

task :clobber_libeio do
  Dir.chdir "ext/libeio" do
    sh "make clean"
  end
end

task :build_libeio do
  Dir.chdir "ext/libeio" do
    sh "./autogen.sh"
    sh "./configure"
    sh "make"
  end unless File.exist?("ext/libeio/.libs/eio.o")
end

RDOC_FILES = FileList["README.rdoc", "ext/eio/eio_ext.c", "lib/eio.rb", "lib/eio/eventmachine.rb", "lib/eio/middleware.rb"]

Rake::RDocTask.new do |rd|
  rd.title = "eio - a libeio wrapper for Ruby"
  rd.main = "README.rdoc"
  rd.rdoc_dir = "doc"
  rd.rdoc_files.include(RDOC_FILES)
end

desc 'Run EIO tests'
Rake::TestTask.new(:test) do |t|
  t.pattern = "test/test_*.rb"
  t.verbose = true
  t.warning = true
end
task :test => :compile

task :default => :test

namespace :bench do
  desc 'Benchmark - eventmachine'
  task :eventmachine do
    ruby 'bench/eventmachine.rb'
  end

  desc 'Benchmark - middleware'
  task :middleware do
    ruby 'bench/middleware.rb'
  end
end