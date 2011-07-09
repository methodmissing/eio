# encoding: utf-8

$:.unshift File.expand_path(File.dirname(__FILE__) + '/../lib')
require 'eio'
require 'rubygems'
require 'eio/eventmachine'

EM.kqueue
EM.set_descriptor_table_size 10_000

class SyncIoConnection < EM::Connection
  def self.stop
    EM.stop
  end

  # Syncronous I/O during request lifetime
  def receive_data(data)
    start = EM.current_time
    f = File.open(__FILE__)
    f.read
    Dir.entries(File.dirname(__FILE__))
    f.close
    send_data (Time.now - start).to_f
    close_connection_after_writing
  end
end

class AsyncIoConnection < EM::Connection
  def self.stop
    EIO.wait
    EM.stop
  end

  # Asyncronous I/O during request lifetime - request finish before all work's complete
  def receive_data(data)
    start = EM.current_time
    EIO.open(__FILE__) do |fd|
      EIO.read(fd) do |buf|
        EIO.readdir(File.dirname(__FILE__)) do |entries|
          EIO.close(fd){ close_connection }
        end
      end
    end
    send_data (Time.now - start).to_f
  end
end

class IoClient < EM::Connection
  REQ_TIMINGS = []
  WORK_TIMINGS = []

  def self.stats(ctx, conns, pid)
    mem = `ps -o rss= -p #{pid}`.to_i
    puts SEP_STR
    puts FMT_STR % (["#{ctx} (#{conns} conns)"].concat(stats_for(REQ_TIMINGS)).concat(stats_for(WORK_TIMINGS)) << mem)
    EM.stop
  end

  def send_data(data)
    @start = Time.now
    super
  end

  def receive_data(data)
    REQ_TIMINGS << data.to_f
  end

  def unbind
    WORK_TIMINGS << (Time.now - @start).to_f
  end

  private
  def self.stats_for(feature)
    total = feature.inject(0){|a,i|a + i}
    avg = (total / feature.size).to_f
    feature.minmax.concat([avg, total])
  ensure
    feature.clear
  end
end

def sync_io_server(conns)
  conn = SyncIoConnection
  EM.fork_reactor do
    trap(:TERM){ conn.stop }
    EM.start_server("0.0.0.0", 8000, conn)
  end
end

def async_io_server(conns)
  conn = AsyncIoConnection
  EM.fork_reactor do
    EIO.max_idle = conns / 10
    EIO.min_parallel = conns / 10
    EIO.max_parallel = conns / 20
    EIO.max_poll_reqs = conns / 50

    trap(:TERM){ conn.stop }
    EIO.eventmachine_handler
    EM.start_server("0.0.0.0", 8000, conn)
  end
end

def run(server, conns = 300)
  server_pid = send(server, conns)
  EM.run do
    EM.add_timer((conns / 50) + 0.5){ IoClient.stats(server, conns, server_pid) }
    conns.times do
      c = EM.connect("0.0.0.0", 8000, IoClient)
      c.send_data('*')
    end
  end
ensure
  Process.kill(:TERM, server_pid)
  sleep 1
end

HEADINGS = [ 'Context', 'Min req time', 'Max req time', 'Avg req time', 'Total req time',
             'Min work time', 'Max work time', 'Avg work time', 'Total work time', 'Memory']
FMT_STR = "| %-30s | %-12.5f | %-12.5f | %-12.5f | %-14.5f | %-13.5f | %-13.5f | %-13.5f | %-15.5f | %-7d |"
SEP_STR = "|#{'-' * 170}|"

puts SEP_STR
puts "| %-30s | %-12s | %-12s | %-12s | %-14s | %-13s | %-13s | %-13s | %-15s | %-7s |" % HEADINGS

run :sync_io_server, 100
run :async_io_server, 100

run :sync_io_server, 200
run :async_io_server, 200

run :sync_io_server, 300
run :async_io_server, 300

puts SEP_STR