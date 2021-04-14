/* Icinga 2 | (c) 2021 Icinga GmbH | GPLv2+ */

#ifndef INFLUXDBCOMMONWRITER_H
#define INFLUXDBCOMMONWRITER_H

#include "perfdata/influxdbcommonwriter-ti.hpp"
#include "icinga/service.hpp"
#include "base/configobject.hpp"
#include "base/tcpsocket.hpp"
#include "base/timer.hpp"
#include "base/tlsstream.hpp"
#include "base/workqueue.hpp"
#include "remote/url.hpp"
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <fstream>

namespace icinga
{

/**
 * An Icinga InfluxDB v1/v2 writer.
 *
 * @ingroup perfdata
 */
class InfluxdbCommonWriter : public ObjectImpl<InfluxdbCommonWriter>
{
public:
	void ValidateHostTemplate(const Lazy<Dictionary::Ptr>& lvalue, const ValidationUtils& utils) override;
	void ValidateServiceTemplate(const Lazy<Dictionary::Ptr>& lvalue, const ValidationUtils& utils) override;

protected:
	WorkQueue m_WorkQueue{10000000, 1};
	std::vector<String> m_DataBuffer;

	void OnConfigLoaded() override;
	void Resume() override;
	void Pause() override;

	virtual boost::beast::http::request<boost::beast::http::string_body> AssembleRequest(String body);
	virtual Url::Ptr AssembleUrl();

private:
	Timer::Ptr m_FlushTimer;

	void CheckResultHandler(const Checkable::Ptr& checkable, const CheckResult::Ptr& cr);
	void CheckResultHandlerWQ(const Checkable::Ptr& checkable, const CheckResult::Ptr& cr);
	void SendMetric(const Checkable::Ptr& checkable, const Dictionary::Ptr& tmpl,
		const String& label, const Dictionary::Ptr& fields, double ts);
	void FlushTimeout();
	void FlushTimeoutWQ();
	void Flush();

	static String EscapeKeyOrTagValue(const String& str);
	static String EscapeValue(const Value& value);

	OptionalTlsStream Connect();

	void AssertOnWorkQueue();

	void ExceptionHandler(boost::exception_ptr exp);
};

}

#endif /* INFLUXDBCOMMONWRITER_H */
