/* Icinga 2 | (c) 2019 Icinga GmbH | GPLv2+ */

#ifndef _WIN32

#include "base/application.hpp"
#include "base/configuration.hpp"
#include "base/dictionary.hpp"
#include "base/exception.hpp"
#include "base/logger.hpp"
#include "base/string.hpp"
#include "cli/daemoncontrol.hpp"
#include "remote/httputility.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/coroutine/exceptions.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <climits>
#include <exception>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace icinga;

class TerminateDaemonControl : public std::exception
{
};

DaemonControl::~DaemonControl()
{
	if (m_Thread.joinable()) {
		Stop();
	}
}

void DaemonControl::Start()
{
	namespace asio = boost::asio;

	{
		String socket = Configuration::InitRunDir + "/icinga2.s";
		UnixStream::endpoint endpoint (socket.CStr());

		m_Acceptor.open(endpoint.protocol());

		// Just to be sure
		(void)unlink(socket.CStr());

		m_Acceptor.bind(endpoint);
		(void)chmod(socket.CStr(), 0700);
		m_Acceptor.listen(INT_MAX);
	}

	asio::spawn(m_IO, [this](asio::yield_context yc) { RunAcceptLoop(yc); });

	m_Thread = std::thread(&DaemonControl::RunEventLoop, this);
	m_WasRunningBeforeFork = true;
}

void DaemonControl::Stop()
{
	namespace asio = boost::asio;

	m_WasRunningBeforeFork = false;
	asio::post(m_IO, []() { throw TerminateDaemonControl(); });
	m_Thread.join();

	auto socket (m_Acceptor.local_endpoint().path());

	m_Acceptor.close();
	(void)unlink(socket.c_str());
}

void DaemonControl::BeforeFork()
{
	namespace asio = boost::asio;

	if (m_WasRunningBeforeFork) {
		asio::post(m_IO, []() { throw TerminateDaemonControl(); });
		m_Thread.join();
	}

	m_IO.notify_fork(m_IO.fork_prepare);
}

void DaemonControl::AfterFork(bool parent)
{
	m_IO.notify_fork(parent ? m_IO.fork_parent : m_IO.fork_child);

	if (m_WasRunningBeforeFork) {
		if (parent) {
			m_Thread = std::thread(&DaemonControl::RunEventLoop, this);
		} else {
			/* Run m_Acceptor's destructor to close the FD in the child
			 * and m_IO's destructor to unwind coroutines
			 */
			this->~DaemonControl();

			// Don't leave the memory uninitialized
			new(this) DaemonControl();
		}
	}
}

void DaemonControl::RunEventLoop()
{
	for (;;) {
		try {
			m_IO.run();
			break;
		} catch (const TerminateDaemonControl&) {
			break;
		} catch (const std::exception& e) {
			Log(LogCritical, "DaemonControl", "Exception during I/O operation!");
			Log(LogDebug, "DaemonControl") << "Exception during I/O operation: " << DiagnosticInformation(e);
		}
	}
}

void DaemonControl::RunAcceptLoop(boost::asio::yield_context& yc)
{
	try {
		for (;;) {
			Connection::Ptr peer = new Connection(m_IO);

			m_Acceptor.async_accept(peer->GetSocket(), yc);
			peer->Start();
		}
	} catch (const boost::coroutines::detail::forced_unwind&) {
		throw;
	} catch (const std::exception& ex) {
		Log(LogCritical, "DaemonControl")
			<< "Cannot accept new connection: " << ex.what();
	}
}

void DaemonControl::Connection::Start()
{
	namespace asio = boost::asio;

	Ptr keepAlive (this);

	asio::spawn(m_Peer.get_executor(), [this, keepAlive](asio::yield_context yc) { ProcessMessages(yc); });
}

template<class Stream>
static inline
bool EnsureValidHeaders(
	Stream& stream,
	boost::beast::flat_buffer& buf,
	boost::beast::http::parser<true, boost::beast::http::string_body>& parser,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	try {
		try {
			http::async_read_header(stream, buf, parser, yc);
		} catch (const boost::system::system_error& ex) {
			/**
			 * Unfortunately there's no way to tell an HTTP protocol error
			 * from an error on a lower layer:
			 *
			 * <https://github.com/boostorg/beast/issues/643>
			 */
			throw std::invalid_argument(ex.what());
		}

		switch (parser.get().version()) {
		case 10:
		case 11:
			break;
		default:
			throw std::invalid_argument("Unsupported HTTP version");
		}
	} catch (const std::invalid_argument& ex) {
		response.result(http::status::bad_request);

		HttpUtility::SendJsonBody(response, nullptr, new Dictionary({
			{ "error", 400 },
			{ "status", String("Bad Request: ") + ex.what() }
		}));

		response.set(http::field::connection, "close");

		http::async_write(stream, response, yc);
		stream.async_flush(yc);

		return false;
	}

	return true;
}

template<class Stream>
static inline
bool EnsureValidBody(
	Stream& stream,
	boost::beast::flat_buffer& buf,
	boost::beast::http::parser<true, boost::beast::http::string_body>& parser,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	try {
		http::async_read(stream, buf, parser, yc);
	} catch (const boost::system::system_error& ex) {
		/**
		 * Unfortunately there's no way to tell an HTTP protocol error
		 * from an error on a lower layer:
		 *
		 * <https://github.com/boostorg/beast/issues/643>
		 */

		response.result(http::status::bad_request);

		HttpUtility::SendJsonBody(response, nullptr, new Dictionary({
			{ "error", 400 },
			{ "status", String("Bad Request: ") + ex.what() }
		}));

		response.set(http::field::connection, "close");

		http::async_write(stream, response, yc);
		stream.async_flush(yc);

		return false;
	}

	return true;
}

template<class Stream>
static inline
void ProcessRequest(
	Stream& stream,
	boost::beast::http::request<boost::beast::http::string_body>& request,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	try {
		HttpUtility::SendJsonError(response, nullptr, 404, "The requested path '" + request.target().to_string() +
			"' could not be found or the request method is not valid for this path.");
	} catch (const boost::coroutines::detail::forced_unwind&) {
		throw;
	} catch (const std::exception& ex) {
		http::response<http::string_body> response;

		HttpUtility::SendJsonError(response, nullptr, 500, "Unhandled exception", DiagnosticInformation(ex));

		http::async_write(stream, response, yc);
		stream.async_flush(yc);

		return;
	}

	http::async_write(stream, response, yc);
	stream.async_flush(yc);
}

const auto l_ServerHeader ("Icinga/" + Application::GetAppVersion());
const boost::system::error_code l_EPipe (boost::system::errc::broken_pipe, boost::system::system_category());

void DaemonControl::Connection::ProcessMessages(boost::asio::yield_context& yc)
{
	namespace beast = boost::beast;
	namespace http = beast::http;

	try {
		beast::flat_buffer buf;

		for (;;) {
			http::parser<true, http::string_body> parser;
			http::response<http::string_body> response;

			parser.header_limit(1024 * 1024);
			parser.body_limit(1024 * 1024);

			response.set(http::field::server, l_ServerHeader);

			if (!EnsureValidHeaders(m_Peer, buf, parser, response, yc)) {
				break;
			}

			auto& request (parser.get());

			Log(LogInformation, "DaemonControl")
				<< "Request: " << request.method_string() << ' ' << request.target()
				<< ", agent: " << request[http::field::user_agent] << ")."; //operator[] - Returns the value for a field, or "" if it does not exist.


			if (!EnsureValidBody(m_Peer, buf, parser, response, yc)) {
				break;
			}

			ProcessRequest(m_Peer, request, response, yc);

			if (request.version() != 11 || request[http::field::connection] == "close") {
				break;
			}
		}
	} catch (const boost::coroutines::detail::forced_unwind&) {
		throw;
	} catch (const std::exception& ex) {
		auto logLevel (LogCritical);

		{
			auto sysErr (dynamic_cast<const boost::system::system_error*>(&ex));

			if (sysErr != nullptr && sysErr->code() == l_EPipe) {
				logLevel = LogNotice;
			}
		}

		Log(logLevel, "DaemonControl")
			<< "Unhandled exception while processing HTTP request: " << ex.what();
	}
}

#endif /* _WIN32 */