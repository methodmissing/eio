# encoding: utf-8

require 'test/unit'
require 'eio'

class TestMiddleware < Test::Unit::TestCase
  def test_wait
    closed = false
    app = Proc.new do |env|
      EIO.open(__FILE__) do |fd|
        EIO.close(fd){ closed = true }
      end
      env
    end

    mw = EIO::Middleware.new(app)
    assert_equal [200, {}, ""], mw.call([200, {}, ""])
    assert closed
  end
end