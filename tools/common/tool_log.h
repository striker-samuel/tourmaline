#pragma once
// Tool stdout/stderr routing: lets host processes (the editor) capture
// tool output line-by-line instead of scraping subprocess pipes.
//
// Usage in a tool TU: replace `std::cout`/`std::cerr` with
// `tool::out()`/`tool::err()` (same std::ostream interface, including
// std::endl/std::unitbuf/std::flush). With no sink set both forward to
// std::cout/std::cerr, so CLI behavior is byte-identical. With a sink
// set, each completed line goes to the callback (isErr selects the
// stream) and nothing touches the real consoles.
//
// One shared instance (NOT per tool): hosts must never run two tools
// concurrently on one process — the editor's cook/bake guards already
// enforce that (m_cookRunning/m_bakeRunning). Set the sink on the
// calling thread before launching the worker, clear it after join.

#include <functional>
#include <iostream>
#include <streambuf>
#include <string>

namespace tool {

using LineSink = std::function<void(const std::string& line, bool isErr)>;

namespace detail {

class LineBuf : public std::streambuf {
public:
	explicit LineBuf(std::ostream& fallback, bool isErr)
		: fallback_(fallback), isErr_(isErr) {}
	void setSink(LineSink sink) { sink_ = std::move(sink); }

protected:
	int_type overflow(int_type ch) override {
		if (ch == traits_type::eof())
			return sync() == 0 ? ch : traits_type::eof();
		if (ch == '\n') {
			emit();
			return ch;
		}
		buf_ += char(ch);
		return ch;
	}
	int sync() override {
		if (!buf_.empty())
			emit();
		return 0;
	}

private:
	void emit() {
		if (sink_) {
			sink_(buf_, isErr_);
		}
		else {
			fallback_ << buf_ << '\n';
			fallback_.flush();
		}
		buf_.clear();
	}
	std::ostream& fallback_;
	const bool isErr_;
	LineSink sink_;
	std::string buf_;
};

inline LineBuf& outBuf() {
	static LineBuf buf(std::cout, false);
	return buf;
}
inline LineBuf& errBuf() {
	static LineBuf buf(std::cerr, true);
	return buf;
}

} // namespace detail

inline std::ostream& out() {
	static std::ostream stream(&detail::outBuf());
	return stream;
}
inline std::ostream& err() {
	static std::ostream stream(&detail::errBuf());
	return stream;
}

// Route both streams' lines to sink (null sink restores console
// forwarding). Call on the supervising thread; the worker's prints
// then arrive line-by-line, like the old pipe drain.
inline void SetLineSink(LineSink sink) {
	detail::outBuf().setSink(sink);
	detail::errBuf().setSink(sink);
}

} // namespace tool
