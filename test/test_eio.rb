# encoding: utf-8

require 'test/unit'
require 'eio'
require 'fileutils' #19

TMP = File.expand_path(File.join(File.dirname(__FILE__), '..', 'tmp'))
SANDBOX = File.join(TMP, 'sandbox')
FileUtils.rm_rf SANDBOX
FileUtils.mkdir_p SANDBOX

class TestEio < Test::Unit::TestCase
  def test_poll
    assert_equal 0, EIO.poll
  end

  def test_version
    assert_equal "0.1", EIO::VERSION
  end

  def test_main_thread_only
    assert_raises ThreadError do
      Thread.new do
        EIO.open(__FILE__) do |fd|
        end
      end.value
      EIO.wait
    end
  end

  def test_fd
    assert_instance_of Fixnum, EIO.fd
  end

  def test_constants
    assert_equal(-4, EIO::PRI_MIN)
    assert_equal 4, EIO::PRI_MAX
    assert_equal 0, EIO::PRI_DEFAULT
  end

  def test_requests
    assert_equal 0, EIO.requests
  end

  def test_ready
    assert_equal 0, EIO.ready
  end

  def test_pending
    assert_equal 0, EIO.pending
  end

  def test_threads
    assert_equal 3, EIO.threads
  end

  def test_set_max_poll_time
    assert_equal 1, EIO.max_poll_time = 1
    assert_equal 0.1, EIO.max_poll_time = 0.1
  end

  def test_set_max_poll_reqs
    assert_equal 1, EIO.max_poll_reqs = 1
  end

  def test_set_min_parallel
    assert_equal 3, EIO.min_parallel = 3
  end

  def test_set_max_parallel
    assert_equal 3, EIO.max_parallel = 3
  end

  def test_set_max_idle
    assert_equal 1, EIO.max_idle = 1
  end

  def test_set_idle_timeout
    assert_equal 1, EIO.idle_timeout = 1
  end

  def test_no_callbacks
    descriptor = nil
    EIO.open(__FILE__) do |fd|
      descriptor = fd
      EIO.close(fd)
    end
    EIO.wait
    assert_raises(Errno::EBADF){ IO.new(descriptor) }
  end

  def test_args
    assert_raises(TypeError){ EIO.open(1) }
    assert_raises(TypeError){ EIO.open(__FILE__, EIO::RDONLY, 0, "") }
    EIO.wait
  end

# OPEN

  def test_open_invalid_path
    assert_raises Errno::ENOENT do
      EIO.open('/non/existent/path'){|fd| }
      EIO.wait
    end
  end

  def test_open_invalid_flags
    assert_raises TypeError do
      EIO.open(__FILE__, :invalid)
    end
  end

  def test_open_invalid_mode
    assert_raises TypeError do
      EIO.open(__FILE__, EIO::RDONLY, :invalid)
    end
  end

  def test_open_close
    closed, descriptor = false, nil
    EIO.open(__FILE__) do |fd|
      descriptor = fd
      EIO.close(fd){ closed = true }
    end
    EIO.wait
    assert closed
    assert_raises(Errno::EBADF){ IO.new(descriptor) }
  end

  def test_open_sync
    fd = EIO.open(__FILE__)
    assert_instance_of Fixnum, fd
    EIO.close(fd)
    assert_raises(Errno::EBADF){ IO.new(fd) }
  end

  def test_open_close_with_proc_cb
    closed = false
    ccb = Proc.new{ closed = true }
    cb = Proc.new do |fd| 
      EIO.close(fd, &ccb)
    end
    EIO.open(__FILE__, &cb)
    EIO.wait
    assert closed
  end

# CLOSE

  def test_close_non_numeric_fd
    assert_raises TypeError do
      EIO.close :symbol
    end
  end

  def test_close_bad_fd
    assert_raises Errno::EBADF do
      EIO.close(900)
      EIO.wait
    end
  end

  def test_close_sync
    descriptor = nil
    EIO.open(__FILE__) do |fd|
      descriptor = fd
      EIO.close(fd)
    end
    EIO.wait
    assert_raises(Errno::EBADF){ IO.new(descriptor) }
  end

# FSYNC

  def test_fsync_non_numeric_fd
    assert_raises TypeError do
      EIO.fsync :symbol
    end
  end

  def test_fsync_bad_fd
    assert_raises Errno::EBADF do
      EIO.fsync(900)
    end
  end

  def test_fsync
    file = File.join(SANDBOX, 'test_fsync.txt')
    buf = "sadklj32oiej23or23"
    fsynced = false
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      EIO.write(fd, buf)
      EIO.fsync(fd) do
        assert_equal buf, IO.read(file)
        fsynced = true
        EIO.close(fd) do
          EIO.unlink(file)
        end
      end
    end
    EIO.wait
    assert fsynced
  end

  def test_fsync_sync
    file = File.join(SANDBOX, 'test_fsync.txt')
    buf = "sadklj32oiej23or23"
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      EIO.write(fd, buf)
      EIO.fsync(fd)
      assert_equal buf, IO.read(file)
      EIO.close(fd) do
        EIO.unlink(file)
      end
    end
    EIO.wait
  end

# FDATASYNC

  def test_fdatasync_non_numeric_fd
    assert_raises TypeError do
      EIO.fdatasync :symbol
    end
  end

  def test_fdatasync_bad_fd
    assert_raises Errno::EBADF do
      EIO.fdatasync(900)
    end
  end

  def test_fdatasync
    file = File.join(SANDBOX, 'test_fdatasync.txt')
    buf = "sadklj32oiej23or23"
    fsynced = false
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      EIO.write(fd, buf)
      EIO.fdatasync(fd) do
        assert_equal buf, IO.read(file)
        fsynced = true
        EIO.close(fd) do
          EIO.unlink(file)
        end
      end
    end
    EIO.wait
    assert fsynced
  end

  def test_fdatasync_sync
    file = File.join(SANDBOX, 'test_fdatasync.txt')
    buf = "sadklj32oiej23or23"
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      EIO.write(fd, buf)
      EIO.fdatasync(fd)
      assert_equal buf, IO.read(file)
      EIO.close(fd) do
        EIO.unlink(file)
      end
    end
    EIO.wait
  end

# READ

  def test_read
    expected = "# encoding: utf-8\n\n"
    data = nil
    EIO.open(__FILE__) do |fd|
      EIO.read(fd, 19) do |buf|
        data = buf
        EIO.close(fd)
      end
    end
    EIO.wait
    assert_equal expected, data
  end

  def test_read_sync
    expected = "# encoding: utf-8\n\n"
    EIO.open(__FILE__) do |fd|
      assert_equal expected, EIO.read(fd, 19)
      EIO.close(fd)
    end
    EIO.wait
  end

  def test_read_with_offset
    expected = "equire "
    data = nil
    EIO.open(__FILE__) do |fd|
      EIO.read(fd, 7, 20) do |buf|
        data = buf
        EIO.close(fd)
      end
    end
    EIO.wait
    assert_equal expected, data
  end

  def test_read_sync_with_offset
    expected = "equire "
    EIO.open(__FILE__) do |fd|
      assert_equal expected, EIO.read(fd, 7, 20)
      EIO.close(fd)
    end
    EIO.wait
  end

  def test_read_bad_fd
    assert_raises Errno::EBADF do
      EIO.read(200) do |buf|
      end
      EIO.wait
    end
  end

  def test_read_invalid_length
    assert_raises TypeError do
      EIO.open(__FILE__) do |fd|
        EIO.read(fd, :invalid)
        EIO.close(fd)
      end
      EIO.wait
    end
  end

  def test_read_invalid_offset
    assert_raises TypeError do
      EIO.open(__FILE__) do |fd|
        EIO.read(fd, 10, :invalid)
        EIO.close(fd)
      end
      EIO.wait
    end
  end

  def test_read_zero_length
    data = nil
    EIO.open(__FILE__) do |fd|
      EIO.read(fd, 0) do |buf|
        data = buf
        EIO.close(fd)
      end
    end
    EIO.wait
    assert_equal 1024, data.size
  end

# READAHEAD

  def test_readahead
    readahead = false
    EIO.open(__FILE__) do |fd|
      EIO.readahead(fd, 100) do
        readahead = true
        EIO.close(fd)
      end
    end
    EIO.wait
    assert readahead
  end

  def test_readahead_bad_fd
    assert_raises Errno::EBADF do
      EIO.readahead(200)
    end
  end

  def test_readahead_invalid_length
    assert_raises TypeError do
      EIO.open(__FILE__) do |fd|
        EIO.readahead(fd, :invalid) do
          EIO.close(fd)
        end
      end
      EIO.wait
    end
  end

  def test_readahead_invalid_offset
    assert_raises TypeError do
      EIO.open(__FILE__) do |fd|
        EIO.readahead(fd, 100, :invalid) do
          EIO.close(fd)
        end
      end
      EIO.wait
    end
  end

  def test_readahead_sync
    EIO.open(__FILE__) do |fd|
      assert_equal 0, EIO.readahead(fd, 100)
      EIO.close(fd)
    end
    EIO.wait
  end

# WRITE

  def test_write_bad_fd
    assert_raises Errno::EBADF do
      EIO.open(__FILE__) do |fd|
        EIO.close(fd)
        EIO.write(200, 'buffer')
      end
      EIO.wait
    end
  end

  def test_write_not_string_buffer
    assert_raises TypeError do
      EIO.open(__FILE__) do |fd|
        EIO.write(fd, :symbol)
      end
      EIO.wait
    end
  end

  def test_write
    file = File.join(SANDBOX, 'test_write.txt')
    buf = "sadklj32oiej23or23"
    descriptor = nil
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      descriptor = fd
      EIO.write(fd, buf) do |written|
        assert_equal written, buf.size
        EIO.read(fd) do |data|
          assert_equal data, buf
          EIO.close(fd)
          EIO.unlink(file)
        end
      end
    end
    EIO.wait
    assert_raises(Errno::EBADF){ IO.new(descriptor) }
  end

  def test_write_sync
    file = File.join(SANDBOX, 'test_write_sync.txt')
    buf = "sadklj32oiej23or23"
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      assert_equal EIO.write(fd, buf), buf.size
      EIO.read(fd) do |data|
        assert_equal data, buf
        EIO.close(fd)
        EIO.unlink(file)
      end
    end
    EIO.wait
  end

  def test_write_partial
    file = File.join(SANDBOX, 'test_write_partial.txt')
    buf = "sadklj32oiej23or23"
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      EIO.write(fd, buf, 4, 0) do |written|
        assert_equal 4, written
        EIO.read(fd) do |data|
          assert_equal "sadk", IO.read(file)
          assert_equal "sadk", data
          EIO.close(fd)
          EIO.unlink(file)
        end
      end
    end
    EIO.wait
  end

  def test_write_partial_sync
    file = File.join(SANDBOX, 'test_write_partial_sync.txt')
    buf = "sadklj32oiej23or23"
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      assert_equal 4, EIO.write(fd, buf, 4, 0)
      EIO.read(fd) do |data|
        assert_equal "sadk", IO.read(file)
        assert_equal "sadk", data
        EIO.close(fd)
        EIO.unlink(file)
      end
    end
    EIO.wait
  end

# SENDFILE

  def test_sendfile_bad_fd
    assert_raises Errno::EBADF do
      EIO.sendfile(300, 400, 0, 8)
    end
  end

  def test_sendfile_invalid_length
    file_out = File.join(SANDBOX, 'test_sendfile_out.txt')
    file_in = File.join(SANDBOX, 'test_sendfile_in.txt')
    assert_raises TypeError do
      EIO.open(file_out, EIO::RDWR | EIO::CREAT) do |fd_in|
        EIO.open(file_in, EIO::RDWR | EIO::CREAT) do |fd_out|
          EIO.sendfile(fd_out, fd_in, :symbol) do |written|
          end
        end
      end
      EIO.wait
    end
  end

  def test_sendfile_invalid_offset
    file_out = File.join(SANDBOX, 'test_sendfile_out.txt')
    file_in = File.join(SANDBOX, 'test_sendfile_in.txt')
    assert_raises TypeError do
      EIO.open(file_out, EIO::RDWR | EIO::CREAT) do |fd_in|
        EIO.open(file_in, EIO::RDWR | EIO::CREAT) do |fd_out|
          EIO.sendfile(fd_out, fd_in, 0, :symbol) do |written|
          end
        end
      end
      EIO.wait
    end
  end

  def test_sendfile
    file_out = File.join(SANDBOX, 'test_sendfile_out.txt')
    file_in = File.join(SANDBOX, 'test_sendfile_in.txt')
    File.open(file_out, "w+"){|f| f << "sendfile" }
    EIO.open(file_out) do |fd_in|
      EIO.open(file_in, EIO::RDWR | EIO::CREAT) do |fd_out|
        EIO.sendfile(fd_out, fd_in, 0, 8) do |written|
          assert_equal 8, written
          EIO.close(fd_out)
          EIO.close(fd_in)
          EIO.unlink(file_in)
          EIO.unlink(file_out)
        end
      end
    end
    EIO.wait
  end

  def test_sendfile_sync
    file_out = File.join(SANDBOX, 'test_sendfile_out.txt')
    file_in = File.join(SANDBOX, 'test_sendfile_in.txt')
    File.open(file_out, "w+"){|f| f << "sendfile" }
    EIO.open(file_out) do |fd_in|
      EIO.open(file_in, EIO::RDWR | EIO::CREAT) do |fd_out|
        assert_equal 8, EIO.sendfile(fd_out, fd_in, 0, 8)
        EIO.close(fd_out)
        EIO.close(fd_in)
        EIO.unlink(file_in)
        EIO.unlink(file_out)
      end
    end
    EIO.wait
  end

# READDIR

  def test_readdir_not_a_string
    assert_raises TypeError do
      EIO.readdir(:symbol) do |entries|
      end
    end
  end

  def test_readdir_invalid_path
    assert_raises Errno::ENOENT do
      EIO.readdir('/invalid/path') do |entries|
      end
      EIO.wait
    end
  end

  def test_readdir
    EIO.readdir(TMP) do |entries|
      assert entries.include?("sandbox")
    end
    EIO.wait
  end

  def test_readdir_sync
    assert EIO.readdir(TMP).include?("sandbox")
  end

# STAT

  def test_stat_not_a_string
    assert_raises TypeError do
      EIO.stat(:symbol) do |stat|
      end
    end
  end

  def test_stat_invalid_path
    assert_raises Errno::ENOENT do
      EIO.stat('/invalid/path') do |stat|
      end
      EIO.wait
    end
  end

  def test_stat
    EIO.stat(__FILE__) do |s|
      assert_instance_of File::Stat, s
      assert_equal 'file', s.ftype
    end
    EIO.wait
  end

  def test_stat_sync
    s = EIO.stat(__FILE__)
    assert_instance_of File::Stat, s
    assert_equal 'file', s.ftype
  end

# MKDIR

  def test_mkdir_not_a_string
    assert_raises TypeError do
      EIO.mkdir(:symbol)
    end
  end

  def test_mkdir_invalid_mode
    path = File.join(SANDBOX, 'test_mkdir_invalid')
    assert_raises TypeError do
      EIO.mkdir(path, :invalid)
    end
  end

  def test_mkdir_rmdir
    path = File.join(SANDBOX, 'test_mkdir')
    EIO.mkdir(path) do
      assert File.directory?(path)
      EIO.rmdir(path) do
        assert !File.directory?(path)
      end
    end
    EIO.wait
  end

  def test_mkdir_rmdir_sync
    path = File.join(SANDBOX, 'test_mkdir')
    EIO.mkdir(path)
    assert File.directory?(path)
    EIO.rmdir(path)
    assert !File.directory?(path)
  end

# RMDIR

  def test_rmdir_not_a_string
    assert_raises TypeError do
      EIO.rmdir(:symbol)
    end
  end

  def test_rmdir_nonexistent_path
    assert_raises Errno::ENOENT do
      EIO.rmdir('/invalid/path') do
      end
      EIO.wait
    end
  end

# UNLINK

  def test_unlink_nonexistent_path
    assert_raises Errno::ENOENT do
      EIO.unlink('/invalid/path/file.txt')
      EIO.wait
    end
  end

  def test_unlink_not_a_string
    assert_raises TypeError do
      EIO.unlink(:symbol)
    end
  end

  def test_unlink
    file = File.join(SANDBOX, 'test_unlink.txt')
    File.new(file, EIO::CREAT).close
    EIO.unlink(file) do
      assert !File.exist?(file)
    end
    EIO.wait
  end

  def test_unlink_sync
    file = File.join(SANDBOX, 'test_unlink.txt')
    File.new(file, EIO::CREAT).close
    EIO.unlink(file)
    assert !File.exist?(file)
    EIO.wait
  end

# READLINK

  def test_readlink_invalid
    assert_raises Errno::ENOENT do
      EIO.readlink('/nonexistent/path') do |path|
      end
      EIO.wait
    end
  end

  def test_readlink_not_a_link
    assert_raises Errno::EINVAL do
      EIO.readlink(__FILE__) do |path|
      end
      EIO.wait
    end
  end

  def test_readlink_not_a_string
    assert_raises TypeError do
      EIO.readlink(:symbol)
    end
  end

  def test_readlink
    file = File.join(SANDBOX, 'test_readlink.txt')
    symlinked = File.join(SANDBOX, 'test_readlinked.txt')
    File.open(file, "w+"){|f| f << "readlink" }
    EIO.symlink(file, symlinked) do
      assert File.exist?(symlinked)
      EIO.readlink(symlinked) do |path|
        assert_equal file, path
        EIO.unlink(symlinked) do
          EIO.unlink(file)
        end
      end
    end
    EIO.wait
  end

  def test_readlink_sync
    file = File.join(SANDBOX, 'test_readlink.txt')
    symlinked = File.join(SANDBOX, 'test_readlinked.txt')
    File.open(file, "w+"){|f| f << "readlink" }
    EIO.symlink(file, symlinked) do
      assert File.exist?(symlinked)
      assert_equal file, EIO.readlink(symlinked)
      EIO.unlink(symlinked) do
        EIO.unlink(file)
      end
    end
    EIO.wait
  end

# RENAME

  def test_rename_not_strings
    assert_raises TypeError do
      EIO.rename(:a, :b)
    end
  end

  def test_rename_invalid_paths
    assert_raises Errno::ENOENT do
      EIO.rename('/invalid/path', '/other/invalid/path')
      EIO.wait
    end
  end

  def test_rename
    file = File.join(SANDBOX, 'test_rename.txt')
    renamed_file = File.join(SANDBOX, 'test_renamed.txt')
    File.new(file, EIO::CREAT).close
    EIO.rename(file, renamed_file) do
      assert !File.exist?(file)
      assert File.exist?(renamed_file)
      EIO.unlink(renamed_file)
    end
    EIO.wait
  end

  def test_rename_sync
    file = File.join(SANDBOX, 'test_rename.txt')
    renamed_file = File.join(SANDBOX, 'test_renamed.txt')
    File.new(file, EIO::CREAT).close
    EIO.rename(file, renamed_file)
    assert !File.exist?(file)
    assert File.exist?(renamed_file)
    EIO.unlink(renamed_file)
    EIO.wait
  end

# CHMOD

  def test_chmod_invalid_path
    assert_raises Errno::ENOENT do
      EIO.chmod('/invalid/path', 0644) do
      end
      EIO.wait
    end
  end

  def test_chmod_invalid_mode
    assert_raises TypeError do
      EIO.chmod(__FILE__, :invalid) do
      end
    end
  end

  def test_chmod_not_a_string_path
    assert_raises TypeError do
      EIO.chmod(:symbol, 0644) do
      end
    end
  end

  def test_chmod
    file = File.join(SANDBOX, 'test_chmod.txt')
    File.new(file, EIO::CREAT).close
    EIO.chmod(file, 0644) do
      # XXX expects "100644"
      assert_equal 33188, File.stat(file).mode
      EIO.unlink(file)
    end
    EIO.wait
  end

  def test_chmod_sync
    file = File.join(SANDBOX, 'test_chmod.txt')
    File.new(file, EIO::CREAT).close
    EIO.chmod(file, 0644)
    # XXX expects "100644"
    assert_equal 33188, File.stat(file).mode
    EIO.unlink(file)
    EIO.wait
  end

# FCHMOD

  def test_fchmod_invalid_fd
    assert_raises Errno::EBADF do
      EIO.fchmod(200, 0644) do
      end
      EIO.wait
    end
  end

  def test_fchmod_invalid_mode
    assert_raises TypeError do
      EIO.open(__FILE__) do |fd|
        EIO.fchmod(fd, :invalid) do
        end
      end
      EIO.wait
    end
  end

  def test_fchmod
    file = File.join(SANDBOX, 'test_fchmod.txt')
    EIO.open(file, EIO::CREAT) do |fd|
      EIO.fchmod(fd, 0644) do
        # XXX expects "100644"
        assert_equal 33188, File.stat(file).mode
        EIO.unlink(file) do
          EIO.close(fd)
        end
      end
    end
    EIO.wait
  end

  def test_fchmod_sync
    file = File.join(SANDBOX, 'test_fchmod.txt')
    EIO.open(file, EIO::CREAT) do |fd|
      EIO.fchmod(fd, 0644)
      # XXX expects "100644"
      assert_equal 33188, File.stat(file).mode
      EIO.unlink(file) do
        EIO.close(fd)
      end
    end
    EIO.wait
  end

# TRUNCATE

  def test_truncate_invalid_path
    assert_raises Errno::ENOENT do
      EIO.truncate('/invalid/path', 0) do
      end
      EIO.wait
    end
  end

  def test_truncate_invalid_offset
    assert_raises TypeError do
      EIO.truncate(__FILE__, :invalid) do
      end
    end
  end

  def test_truncate
    file = File.join(SANDBOX, 'test_truncate.txt')
    File.open(file, "w+"){|f| f << "truncate" }
    EIO.truncate(file) do
      assert_equal 0, File.size(file)
      EIO.unlink(file)
    end
    EIO.wait
  end

  def test_truncate_sync
    file = File.join(SANDBOX, 'test_truncate.txt')
    File.open(file, "w+"){|f| f << "truncate" }
    EIO.truncate(file)
    assert_equal 0, File.size(file)
    EIO.unlink(file)
  end

# FTRUNCATE

  def test_ftruncate_invalid_fd
    assert_raises Errno::EBADF do
      EIO.ftruncate(200, 0) do
      end
      EIO.wait
    end
  end

  def test_ftruncate_invalid_offset
    assert_raises TypeError do
      EIO.open(__FILE__) do |fd|
        EIO.ftruncate(fd, :invalid) do
        end
      end
      EIO.wait
    end
  end

  def test_ftruncate
    file = File.join(SANDBOX, 'test_ftruncate.txt')
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      EIO.ftruncate(fd) do
        assert_equal 0, File.size(file)
        EIO.unlink(file) do
          EIO.close(fd)
        end
      end
    end
    EIO.wait
  end

  def test_ftruncate_sync
    file = File.join(SANDBOX, 'test_ftruncate.txt')
    EIO.open(file, EIO::RDWR | EIO::CREAT) do |fd|
      EIO.ftruncate(fd)
      assert_equal 0, File.size(file)
      EIO.unlink(file) do
        EIO.close(fd)
      end
    end
    EIO.wait
  end

# CHOWN

  def test_chown_invalid_path
    assert_raises Errno::ENOENT do
      EIO.chown('/invalid/path') do
      end
      EIO.wait
    end
  end

  def test_chown_not_a_string
    assert_raises TypeError do
      EIO.chown(:symbol)
    end
  end

  def test_chown_invalid_perms
    assert_raises TypeError do
      EIO.chown(__FILE__, :a, :b)
    end
  end

  def test_chown
    file = File.join(SANDBOX, 'test_chown.txt')
    File.new(file, EIO::CREAT).close
    EIO.chown(file) do
      assert_equal 502, File.stat(file).uid
      assert_equal 502, File.stat(file).gid
      EIO.unlink(file)
    end
    EIO.wait
  end

  def test_chown_sync
    file = File.join(SANDBOX, 'test_chown.txt')
    File.new(file, EIO::CREAT).close
    EIO.chown(file)
    assert_equal 502, File.stat(file).uid
    assert_equal 502, File.stat(file).gid
    EIO.unlink(file)
    EIO.wait
  end

# FCHOWN

  def test_fchown_bad_fd
    assert_raises Errno::EBADF do
      EIO.fchown(200) do
      end
      EIO.wait
    end
  end

  def test_fchown_invalid_perms
    assert_raises TypeError do
      EIO.open(__FILE__) do |fd|
        EIO.fchown(fd, :a, :b)
      end
      EIO.wait
    end
  end

  def test_fchown
    file = File.join(SANDBOX, 'test_fchown.txt')
    EIO.open(file, EIO::CREAT) do |fd|
      EIO.fchown(fd) do
        assert_equal 502, File.stat(file).uid
        assert_equal 502, File.stat(file).gid
        EIO.unlink(file) do
          EIO.close(fd)
        end
      end
    end
    EIO.wait
  end

  def test_fchown_sync
    file = File.join(SANDBOX, 'test_fchown.txt')
    EIO.open(file, EIO::CREAT) do |fd|
      EIO.fchown(fd)
      assert_equal 502, File.stat(file).uid
      assert_equal 502, File.stat(file).gid
      EIO.unlink(file) do
        EIO.close(fd)
      end
    end
    EIO.wait
  end

# LINK

  def test_link_invalid_paths
    assert_raises Errno::ENOENT do
      EIO.link('/invalid/path/a', '/invalid/path/b')
      EIO.wait
    end
  end

  def test_link_not_strings
    assert_raises TypeError do
      EIO.link(:a, :b)
    end
  end

  def test_link
    file = File.join(SANDBOX, 'test_link.txt')
    linked = File.join(SANDBOX, 'test_linked.txt')
    File.open(file, "w+"){|f| f << "link" }
    EIO.link(file, linked) do
      assert File.exist?(linked)
      assert_equal "link", IO.read(linked)
      EIO.unlink(linked) do
        EIO.unlink(file)
      end
    end
    EIO.wait
  end

  def test_link_sync
    file = File.join(SANDBOX, 'test_link.txt')
    linked = File.join(SANDBOX, 'test_linked.txt')
    File.open(file, "w+"){|f| f << "link" }
    EIO.link(file, linked)
    assert File.exist?(linked)
    assert_equal "link", IO.read(linked)
    EIO.unlink(linked) do
      EIO.unlink(file)
    end
    EIO.wait
  end

# SYMLINK

  def test_symlink_invalid_paths
    assert_raises Errno::ENOENT do
      EIO.symlink('/invalid/path/a', '/invalid/path/b')
      EIO.wait
    end
  end

  def test_symlink_not_strings
    assert_raises TypeError do
      EIO.symlink(:a, :b)
    end
  end

  def test_symlink
    file = File.join(SANDBOX, 'test_symlink.txt')
    symlinked = File.join(SANDBOX, 'test_symlinked.txt')
    File.open(file, "w+"){|f| f << "symlink" }
    EIO.symlink(file, symlinked) do
      assert File.exist?(symlinked)
      assert_equal "symlink", IO.read(symlinked)
      EIO.unlink(symlinked) do
        EIO.unlink(file)
      end
    end
    EIO.wait
  end

  def test_symlink_sync
    file = File.join(SANDBOX, 'test_symlink.txt')
    symlinked = File.join(SANDBOX, 'test_symlinked.txt')
    File.open(file, "w+"){|f| f << "symlink" }
    EIO.symlink(file, symlinked)
    assert File.exist?(symlinked)
    assert_equal "symlink", IO.read(symlinked)
    EIO.unlink(symlinked) do
      EIO.unlink(file)
    end
    EIO.wait
  end

  def test_request
    req = EIO.open(__FILE__) do |fd|
      EIO.close(fd)
    end
    EIO.wait
    assert_instance_of EIO::Request, req
    assert_equal 0, req.errno
    assert_equal EIO::Request::OPEN, req.type
    assert_equal EIO::PRI_DEFAULT, req.priority
  end

  def test_cancel_request
    cancelled = true
    EIO.open(__FILE__) do |fd|
      cancelled = false
    end.cancel
    EIO.wait
    assert cancelled
  end

  def test_complete
    req = EIO.open(__FILE__) do |fd|
    end
    EIO.wait
    assert req.complete?
  end
end