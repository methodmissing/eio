# encoding: utf-8

require 'test/unit'
require 'eio'

begin
require 'rubygems'
require 'eio/eventmachine'
rescue LoadError
end 

class TestEventmachine < Test::Unit::TestCase
  def test_eventmachine
    EM.run do
      EIO.eventmachine_handler
      EIO.open(__FILE__) do |fd|
        EIO.read(fd) do |data|
          EM.stop
        end
      end
    end
  end
end if defined?(::EM)
