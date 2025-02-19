// Copyright 2023 Northern.tech AS
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.

#include <common/io.hpp>

#include <common/config.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <istream>
#include <memory>
#include <streambuf>
#include <vector>
#include <fstream>

namespace mender {
namespace common {
namespace io {

namespace error = mender::common::error;
namespace expected = mender::common::expected;

void AsyncReader::RepeatedAsyncRead(
	vector<uint8_t>::iterator start,
	vector<uint8_t>::iterator end,
	RepeatedAsyncIoHandler handler) {
	class Functor {
	public:
		AsyncReader &reader;
		vector<uint8_t>::iterator start;
		vector<uint8_t>::iterator end;
		RepeatedAsyncIoHandler handler;
		void ScheduleNextRead(Repeat repeat) {
			while (repeat == Repeat::Yes) {
				auto err = reader.AsyncRead(start, end, *this);
				if (err == error::NoError) {
					break;
				} else {
					repeat = handler(expected::unexpected(err));
				}
			}
		}
		void operator()(ExpectedSize num_read) {
			auto repeat = handler(num_read);
			ScheduleNextRead(repeat);
		}
	};
	Functor func {*this, start, end, handler};
	func.ScheduleNextRead(Repeat::Yes);
}

Error Copy(Writer &dst, Reader &src) {
	vector<uint8_t> buffer(MENDER_BUFSIZE);
	return Copy(dst, src, buffer);
}

Error Copy(Writer &dst, Reader &src, vector<uint8_t> &buffer) {
	while (true) {
		auto r_result = src.Read(buffer.begin(), buffer.end());
		if (!r_result) {
			return r_result.error();
		} else if (r_result.value() == 0) {
			return NoError;
		} else if (r_result.value() > buffer.size()) {
			return error::MakeError(
				error::ProgrammingError,
				"Read returned more bytes than requested. This is a bug in the Read function.");
		}

		auto w_result = dst.Write(buffer.cbegin(), buffer.cbegin() + r_result.value());
		if (!w_result) {
			return w_result.error();
		} else if (w_result.value() == 0) {
			// Should this even happen?
			return Error(std::error_condition(std::errc::io_error), "Zero write when copying data");
		} else if (r_result.value() != w_result.value()) {
			return Error(
				std::error_condition(std::errc::io_error), "Short write when copying data");
		}
	}
}

struct CopyData {
	CopyData(int64_t limit) :
		buf(MENDER_BUFSIZE),
		limit {limit} {
	}

	vector<uint8_t> buf;
	int64_t copied {0};
	int64_t limit;
};

void AsyncCopy(
	Writer &dst, AsyncReader &src, function<void(Error)> finished_handler, int64_t stop_after) {
	AsyncCopy(
		WriterPtr(&dst, [](Writer *) {}),
		AsyncReaderPtr(&src, [](AsyncReader *) {}),
		finished_handler,
		stop_after);
}

void AsyncCopy(
	WriterPtr dst, AsyncReaderPtr src, function<void(Error)> finished_handler, int64_t stop_after) {
	auto data = make_shared<CopyData>(stop_after);
	class Functor {
	public:
		void operator()(ExpectedSize size) {
			if (!size) {
				CallFinishedHandler(size.error());
				return;
			}


			if (*size == 0) {
				CallFinishedHandler(error::NoError);
				return;
			}

			auto n = writer->Write(data->buf.begin(), data->buf.begin() + *size);
			if (!n) {
				CallFinishedHandler(n.error());
				return;
			} else if (*n != *size) {
				CallFinishedHandler(Error(
					make_error_condition(std::errc::io_error), "Short write when copying data"));
				return;
			}

			data->copied += *n;

			size_t to_copy = static_cast<size_t>(
				min(data->limit - data->copied, static_cast<int64_t>(data->buf.size())));
			if (to_copy == 0) {
				CallFinishedHandler(error::NoError);
				return;
			}

			auto err = reader->AsyncRead(
				data->buf.begin(),
				data->buf.begin() + to_copy,
				Functor {writer, reader, data, finished_handler});
			if (err != error::NoError) {
				CallFinishedHandler(err);
			}
		}

		void CallFinishedHandler(const error::Error &err) {
			// Sometimes the functor is kept on the event loop longer than we expect. It
			// will eventually be destroyed, but destroy what we own now, so that
			// we don't keep references to them after we're done.
			auto handler = finished_handler;
			*this = {};
			handler(err);
		}

		WriterPtr writer;
		AsyncReaderPtr reader;
		shared_ptr<CopyData> data;
		function<void(Error)> finished_handler;
	};
	size_t to_copy = static_cast<size_t>(min(data->limit, static_cast<int64_t>(data->buf.size())));
	auto err = src->AsyncRead(
		data->buf.begin(), data->buf.begin() + to_copy, Functor {dst, src, data, finished_handler});
	if (err != error::NoError) {
		finished_handler(err);
	}
}

void AsyncCopy(
	AsyncWriter &dst, Reader &src, function<void(Error)> finished_handler, int64_t stop_after) {
	AsyncCopy(
		AsyncWriterPtr(&dst, [](AsyncWriter *) {}),
		ReaderPtr(&src, [](Reader *) {}),
		finished_handler,
		stop_after);
}

void AsyncCopy(
	AsyncWriterPtr dst, ReaderPtr src, function<void(Error)> finished_handler, int64_t stop_after) {
	auto data = make_shared<CopyData>(stop_after);

	class Functor {
	public:
		void operator()(ExpectedSize exp_written) {
			if (!exp_written) {
				CallFinishedHandler(exp_written.error());
				return;
			} else if (exp_written.value() != expected_written) {
				CallFinishedHandler(Error(
					make_error_condition(std::errc::io_error), "Short write when copying data"));
				return;
			}

			data->copied += *exp_written;

			size_t to_copy = static_cast<size_t>(
				min(data->limit - data->copied, static_cast<int64_t>(data->buf.size())));
			if (to_copy == 0) {
				CallFinishedHandler(error::NoError);
				return;
			}

			auto exp_read = reader->Read(data->buf.begin(), data->buf.begin() + to_copy);
			if (!exp_read) {
				CallFinishedHandler(exp_read.error());
				return;
			}
			auto &read = exp_read.value();

			if (read == 0) {
				CallFinishedHandler(error::NoError);
				return;
			}

			auto err = writer->AsyncWrite(
				data->buf.begin(),
				data->buf.begin() + read,
				Functor {writer, reader, finished_handler, data, read});
			if (err != error::NoError) {
				CallFinishedHandler(err);
			}
		}

		void CallFinishedHandler(const error::Error &err) {
			// Sometimes the functor is kept on the event loop longer than we expect. It
			// will eventually be destroyed, but destroy what we own now, so that
			// we don't keep references to them after we're done.
			auto handler = finished_handler;
			*this = {};
			handler(err);
		}

		AsyncWriterPtr writer;
		ReaderPtr reader;
		function<void(Error)> finished_handler;
		shared_ptr<CopyData> data;
		size_t expected_written;
	};

	Functor initial {dst, src, finished_handler, data, 0};
	initial(0);
}

void AsyncCopy(
	AsyncWriter &dst,
	AsyncReader &src,
	function<void(Error)> finished_handler,
	int64_t stop_after) {
	AsyncCopy(
		AsyncWriterPtr(&dst, [](AsyncWriter *) {}),
		AsyncReaderPtr(&src, [](AsyncReader *) {}),
		finished_handler,
		stop_after);
}

class AsyncCopyReaderFunctor {
public:
	void operator()(io::ExpectedSize exp_size);

	void CallFinishedHandler(const error::Error &err) {
		// Sometimes the functor is kept on the event loop longer than we expect. It will
		// eventually be destroyed, but destroy what we own now, so that we don't keep
		// references to them after we're done.
		auto handler = finished_handler;
		*this = {};
		handler(err);
	}

	AsyncWriterPtr writer;
	AsyncReaderPtr reader;
	function<void(Error)> finished_handler;
	shared_ptr<CopyData> data;
};

class AsyncCopyWriterFunctor {
public:
	void operator()(io::ExpectedSize exp_size);

	void CallFinishedHandler(const error::Error &err) {
		// Sometimes the functor is kept on the event loop longer than we expect. It will
		// eventually be destroyed, but destroy what we own now, so that we don't keep
		// references to them after we're done.
		auto handler = finished_handler;
		*this = {};
		handler(err);
	}

	AsyncWriterPtr writer;
	AsyncReaderPtr reader;
	function<void(Error)> finished_handler;
	shared_ptr<CopyData> data;
	size_t expected_written;
};

void AsyncCopyReaderFunctor::operator()(io::ExpectedSize exp_size) {
	if (!exp_size) {
		CallFinishedHandler(exp_size.error());
		return;
	}
	if (exp_size.value() == 0) {
		CallFinishedHandler(error::NoError);
		return;
	}

	auto err = writer->AsyncWrite(
		data->buf.begin(),
		data->buf.begin() + exp_size.value(),
		AsyncCopyWriterFunctor {writer, reader, finished_handler, data, exp_size.value()});
	if (err != error::NoError) {
		CallFinishedHandler(err);
	}
}

void AsyncCopyWriterFunctor::operator()(io::ExpectedSize exp_size) {
	if (!exp_size) {
		CallFinishedHandler(exp_size.error());
		return;
	}
	if (exp_size.value() != expected_written) {
		CallFinishedHandler(
			error::Error(make_error_condition(errc::io_error), "Short write in AsyncCopy"));
		return;
	}

	data->copied += *exp_size;

	size_t to_copy = static_cast<size_t>(
		min(data->limit - data->copied, static_cast<int64_t>(data->buf.size())));
	if (to_copy == 0) {
		CallFinishedHandler(error::NoError);
		return;
	}

	auto err = reader->AsyncRead(
		data->buf.begin(),
		data->buf.begin() + to_copy,
		AsyncCopyReaderFunctor {writer, reader, finished_handler, data});
	if (err != error::NoError) {
		CallFinishedHandler(err);
	}
}

void AsyncCopy(
	AsyncWriterPtr dst,
	AsyncReaderPtr src,
	function<void(Error)> finished_handler,
	int64_t stop_after) {
	auto data = make_shared<CopyData>(stop_after);

	size_t to_copy = static_cast<size_t>(min(data->limit, static_cast<int64_t>(data->buf.size())));
	auto err = src->AsyncRead(
		data->buf.begin(),
		data->buf.begin() + to_copy,
		AsyncCopyReaderFunctor {dst, src, finished_handler, data});
	if (err != error::NoError) {
		finished_handler(err);
	}
}

ExpectedSize ByteReader::Read(vector<uint8_t>::iterator start, vector<uint8_t>::iterator end) {
	assert(end > start);
	Vsize max_read {emitter_->size() - bytes_read_};
	Vsize iterator_size {static_cast<Vsize>(end - start)};
	Vsize bytes_to_read {min(iterator_size, max_read)};
	auto it = next(emitter_->begin(), bytes_read_);
	std::copy_n(it, bytes_to_read, start);
	bytes_read_ += bytes_to_read;
	return bytes_to_read;
}

void ByteReader::Rewind() {
	bytes_read_ = 0;
}

void ByteWriter::SetUnlimited(bool enabled) {
	unlimited_ = enabled;
}

ExpectedSize ByteWriter::Write(
	vector<uint8_t>::const_iterator start, vector<uint8_t>::const_iterator end) {
	assert(end > start);
	Vsize max_write {receiver_->size() - bytes_written_};
	if (max_write == 0 && !unlimited_) {
		return expected::unexpected(Error(make_error_condition(errc::no_space_on_device), ""));
	}
	Vsize iterator_size {static_cast<Vsize>(end - start)};
	Vsize bytes_to_write;
	if (unlimited_) {
		bytes_to_write = iterator_size;
		if (max_write < bytes_to_write) {
			receiver_->resize(bytes_written_ + bytes_to_write);
			max_write = bytes_to_write;
		}
	} else {
		bytes_to_write = min(iterator_size, max_write);
	}
	auto it = next(receiver_->begin(), bytes_written_);
	std::copy_n(start, bytes_to_write, it);
	bytes_written_ += bytes_to_write;
	return bytes_to_write;
}


ExpectedSize StreamWriter::Write(
	vector<uint8_t>::const_iterator start, vector<uint8_t>::const_iterator end) {
	os_->write(reinterpret_cast<const char *>(&*start), end - start);
	if (!(*(os_.get()))) {
		return expected::unexpected(Error(make_error_condition(errc::io_error), ""));
	}
	return end - start;
}

class ReaderStreamBuffer : public streambuf {
public:
	ReaderStreamBuffer(Reader &reader) :
		reader_ {reader},
		buf_(buf_size_) {};
	streambuf::int_type underflow() override;

private:
	static const Vsize buf_size_ = MENDER_BUFSIZE;
	Reader &reader_;
	vector<uint8_t> buf_;
};

streambuf::int_type ReaderStreamBuffer::underflow() {
	// eback -- pointer to the first char (byte)
	// gptr  -- pointer to the current char (byte)
	// egptr -- pointer past the last char (byte)

	// This function is only called if gptr() == nullptr or gptr() >= egptr(),
	// i.e. if there's nothing more to read.
	if (this->gptr() >= this->egptr()) {
		errno = 0;
		auto ex_n_read = reader_.Read(buf_.begin(), buf_.end());
		streamsize n_read;
		if (ex_n_read) {
			n_read = ex_n_read.value();
		} else {
			// There is no way to return an error from underflow(), generally
			// the streams only care about how much data was read. No data or
			// less data then requested by the caller of istream.read() means
			// eofbit and failbit are set. If the user code wants to get the
			// error or check if there was an error, it needs to check errno.
			//
			// So as long as we don't clear errno after a failure in the
			// reader_.Read() above, error handling works as usual and returning
			// eof below is all that needs to happen here.
			//
			// In case errno is not set for some reason, let's try to get it
			// from the error with a fallback to a generic I/O error.
			if (errno == 0) {
				if (ex_n_read.error().code.category() == generic_category()) {
					errno = ex_n_read.error().code.value();
				} else {
					errno = EIO;
				}
			}
			n_read = 0;
		}

		streambuf::char_type *first = reinterpret_cast<streambuf::char_type *>(buf_.data());

		// set eback, gptr, egptr
		this->setg(first, first, first + n_read);
	}

	return this->gptr() == this->egptr() ? std::char_traits<char>::eof()
										 : std::char_traits<char>::to_int_type(*this->gptr());
};

/**
 * A variant of the #istream class that takes ownership of the #streambuf buffer
 * created for it.
 *
 * @note Base #istream is designed to work on shared buffers so it doesn't
 *       destruct/delete the buffer.
 */
class istreamWithUniqueBuffer : public istream {
public:
	// The unique_ptr, &&buf and std::move() model this really nicely -- a
	// unique_ptr rvalue (i.e. temporary) is required and it's moved into the
	// object. The default destructor then takes care of cleaning up properly.
	istreamWithUniqueBuffer(unique_ptr<streambuf> &&buf) :
		istream(buf.get()),
		buf_ {std::move(buf)} {};

private:
	unique_ptr<streambuf> buf_;
};

unique_ptr<istream> Reader::GetStream() {
	return unique_ptr<istream>(
		new istreamWithUniqueBuffer(unique_ptr<ReaderStreamBuffer>(new ReaderStreamBuffer(*this))));
};

ExpectedIfstream OpenIfstream(const string &path) {
	ifstream is;
	errno = 0;
	is.open(path);
	if (!is) {
		int io_errno = errno;
		return ExpectedIfstream(expected::unexpected(error::Error(
			generic_category().default_error_condition(io_errno),
			"Failed to open '" + path + "' for reading")));
	}
	return ExpectedIfstream(std::move(is));
}

ExpectedSharedIfstream OpenSharedIfstream(const string &path) {
	auto exp_is = OpenIfstream(path);
	if (!exp_is) {
		return expected::unexpected(exp_is.error());
	}
	return make_shared<ifstream>(std::move(exp_is.value()));
}

ExpectedOfstream OpenOfstream(const string &path, bool append) {
	ofstream os;
	errno = 0;
	os.open(path, append ? ios::app : ios::out);
	if (!os) {
		int io_errno = errno;
		return ExpectedOfstream(expected::unexpected(error::Error(
			generic_category().default_error_condition(io_errno),
			"Failed to open '" + path + "' for writing")));
	}
	return os;
}

ExpectedSharedOfstream OpenSharedOfstream(const string &path, bool append) {
	auto exp_is = OpenOfstream(path, append);
	if (!exp_is) {
		return expected::unexpected(exp_is.error());
	}
	return make_shared<ofstream>(std::move(exp_is.value()));
}

error::Error WriteStringIntoOfstream(ofstream &os, const string &data) {
	errno = 0;
	os.write(data.data(), data.size());
	if (os.bad() || os.fail()) {
		int io_errno = errno;
		return error::Error(
			std::generic_category().default_error_condition(io_errno),
			"Failed to write data into the stream");
	}

	return error::NoError;
}

ExpectedSize StreamReader::Read(vector<uint8_t>::iterator start, vector<uint8_t>::iterator end) {
	is_->read(reinterpret_cast<char *>(&*start), end - start);
	if (!is_) {
		int io_error = errno;
		return expected::unexpected(
			Error(std::generic_category().default_error_condition(io_error), ""));
	}
	return is_->gcount();
}

error::Error FileReader::Rewind() {
	if (!is_) {
		auto ex_is = OpenSharedIfstream(path_);
		if (!ex_is) {
			return ex_is.error();
		}
		is_ = ex_is.value();
	}
	if (!(*is_)) {
		return Error(std::error_condition(std::errc::io_error), "Bad stream, cannot rewind");
	}
	errno = 0;
	is_->seekg(0, ios::beg);
	int io_errno = errno;
	if (!(*is_)) {
		return Error(
			generic_category().default_error_condition(io_errno),
			"Failed to seek to the beginning of the stream");
	}
	return error::NoError;
}

ExpectedSize BufferedReader::Read(vector<uint8_t>::iterator start, vector<uint8_t>::iterator end) {
	if (rewind_done_ && !rewind_consumed_) {
		// Read from the buffer
		auto ex_bytes_read = buffer_reader_.Read(start, end);
		if (!ex_bytes_read) {
			return ex_bytes_read;
		}

		Vsize bytes_read_buffer = ex_bytes_read.value();

		// Because we track the number of bytes, we should never hit EOF.
		AssertOrReturnUnexpected(bytes_read_buffer > 0);
		AssertOrReturnUnexpected(buffer_remaining_ >= bytes_read_buffer);

		buffer_remaining_ -= bytes_read_buffer;

		// When out of bytes, continue with reading from the wrapped reader
		if (buffer_remaining_ == 0) {
			rewind_consumed_ = true;
			if (stop_done_) {
				buffer_.clear();
			}
		}

		return bytes_read_buffer;
	}

	// Read from the wrapped reader and save copy into the buffer
	auto bytes_read = wrapped_reader_.Read(start, end);
	if (!bytes_read) {
		return bytes_read;
	}
	if (!stop_done_) {
		buffer_.insert(buffer_.end(), start, start + bytes_read.value());
	}
	return bytes_read;
}

ExpectedSize BufferedReader::Rewind() {
	if (stop_done_ && rewind_done_) {
		return expected::unexpected(error::Error(
			make_error_condition(errc::io_error), "Buffering was stopped, cannot rewind anymore"));
	}
	buffer_reader_.Rewind();
	rewind_done_ = true;
	buffer_remaining_ = buffer_.size();
	rewind_consumed_ = (buffer_remaining_ == 0);
	return buffer_remaining_;
}

ExpectedSize BufferedReader::StopBufferingAndRewind() {
	auto result = Rewind();
	stop_done_ = true;
	return result;
}

error::Error BufferedReader::StopBufferingAndDiscard() {
	if (rewind_done_ && !rewind_consumed_) {
		return error::Error(
			make_error_condition(errc::io_error), "Cannot stop buffering, pending rewind read");
	}
	stop_done_ = true;
	rewind_consumed_ = true;
	buffer_.clear();
	return error::NoError;
}

error::Error AsyncBufferedReader::AsyncRead(
	vector<uint8_t>::iterator start, vector<uint8_t>::iterator end, AsyncIoHandler handler) {
	if (rewind_done_ && !rewind_consumed_) {
		// Read from the buffer
		auto ex_bytes_read = buffer_reader_.Read(start, end);
		if (!ex_bytes_read) {
			handler(ex_bytes_read);
			return ex_bytes_read.error();
		}

		Vsize bytes_read_buffer = ex_bytes_read.value();

		// Because we track the number of bytes, we should never hit EOF.
		AssertOrReturnError(bytes_read_buffer > 0);
		AssertOrReturnError(buffer_remaining_ >= bytes_read_buffer);

		buffer_remaining_ -= bytes_read_buffer;

		// When out of bytes, continue with reading from the wrapped reader
		if (buffer_remaining_ == 0) {
			rewind_consumed_ = true;
			if (stop_done_) {
				buffer_.clear();
			}
		}

		handler(ex_bytes_read);
		return error::NoError;
	}

	// Read from the wrapped reader and save copy into the buffer
	auto wrapper_handler = [this, start, handler](ExpectedSize result) {
		if (!result) {
			handler(result);
			return;
		}
		if (!stop_done_) {
			buffer_.insert(buffer_.end(), start, start + result.value());
		}
		handler(result);
	};
	auto err = wrapped_reader_.AsyncRead(start, end, wrapper_handler);
	return err;
}

ExpectedSize AsyncBufferedReader::Rewind() {
	if (stop_done_ && rewind_done_) {
		return expected::unexpected(error::Error(
			make_error_condition(errc::io_error), "Buffering was stopped, cannot rewind anymore"));
	}
	buffer_reader_.Rewind();
	rewind_done_ = true;
	buffer_remaining_ = buffer_.size();
	rewind_consumed_ = (buffer_remaining_ == 0);
	return buffer_remaining_;
}

ExpectedSize AsyncBufferedReader::StopBufferingAndRewind() {
	auto result = Rewind();
	stop_done_ = true;
	return result;
}

error::Error AsyncBufferedReader::StopBufferingAndDiscard() {
	if (rewind_done_ && !rewind_consumed_) {
		return error::Error(
			make_error_condition(errc::io_error), "Cannot stop buffering, pending rewind read");
	}
	stop_done_ = true;
	rewind_consumed_ = true;
	buffer_.clear();
	return error::NoError;
}

} // namespace io
} // namespace common
} // namespace mender
