# encoding: utf-8

require 'eio'

# The call to EIO.wait blocks until callbacks for all completed requests have been invoked. This workflow
# is comparable to a 100m race with each line representing a libeio request and EIO.wait being the
# finishing line / completion barrier. Each I/O operation may be scheduled on a different core and will
# complete in parallel, proportional to the slowest request, with some minor overhead to boot.

class EIO::Middleware
  def initialize(app, opts = {})
     @app = app
     @options = opts
  end

  def call(env)
    ret = @app.call(env)
    EIO.wait # flush the libeio request queue (blocks until all requests have been processed)
    ret
  end
end