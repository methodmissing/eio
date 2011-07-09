# encoding: utf-8

$:.unshift File.expand_path(File.dirname(__FILE__) + '/../lib')
require 'benchmark'
require 'eio'
require 'eio/middleware'

blocking_app = Proc.new{ |env|
  f = File.open(__FILE__)
  f.read
  Dir.entries(File.dirname(__FILE__))
  f.close
}

eio_app = Proc.new{ |env|
  EIO.open(__FILE__) do |fd|
    EIO.read(fd) do |buf|
      EIO.readdir(File.dirname(__FILE__)) do |entries|
        EIO.close(fd)
      end
    end
  end
}

Runs = 1000
Env = [200, {}, ""]

@mw = Proc.new{|env| blocking_app.call(env) }
@eio_mw = EIO::Middleware.new(eio_app)

Benchmark.bmbm do |results|
  results.report("Blocking middleware") { Runs.times{ @mw.call(Env) } }
  results.report("EIO middleware") { Runs.times{ @eio_mw.call(Env) } }
end
