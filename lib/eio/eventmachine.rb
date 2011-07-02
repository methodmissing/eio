# encoding: utf-8

require 'eio'
require 'eventmachine'

# The Eventmachine handler watches the read end of a pipe which wakes up the event loop whenever there's
# results to process. This is entirely driven from libeio which writes a char to the write end of the pipe
# to wake up the loop. The EIO.poll callback will fire as many times as it needs to as we don't read data # from the pipe through the reactor. A separate callback invoked by libeio will read the char and clear
# it's readable state.

module EM::EioHandler
  def notify_readable
    EIO.poll
  end
end

module EIO

  # Registers the read end of a pipe with Eventmachine which wakes up the event loop whenever there's
  # results to process.
  def self.eventmachine_handler
    EM.watch(EIO.fd, EM::EioHandler){ |c| c.notify_readable = true }
  end
end