/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "perfdata/influxdbwriter.hpp"
#include "perfdata/influxdbwriter-ti.cpp"
#include "base/base64.hpp"
#include "remote/url.hpp"
#include "base/configtype.hpp"
#include "base/perfdatavalue.hpp"
#include "base/statsfunction.hpp"
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <utility>

using namespace icinga;

REGISTER_TYPE(InfluxdbWriter);

REGISTER_STATSFUNCTION(InfluxdbWriter, &InfluxdbWriter::StatsFunc);

void InfluxdbWriter::StatsFunc(const Dictionary::Ptr& status, const Array::Ptr& perfdata)
{
	DictionaryData nodes;

	for (const InfluxdbWriter::Ptr& influxdbwriter : ConfigType::GetObjectsByType<InfluxdbWriter>()) {
		size_t workQueueItems = influxdbwriter->m_WorkQueue.GetLength();
		double workQueueItemRate = influxdbwriter->m_WorkQueue.GetTaskCount(60) / 60.0;
		size_t dataBufferItems = influxdbwriter->m_DataBuffer.size();

		nodes.emplace_back(influxdbwriter->GetName(), new Dictionary({
			{ "work_queue_items", workQueueItems },
			{ "work_queue_item_rate", workQueueItemRate },
			{ "data_buffer_items", dataBufferItems }
		}));

		perfdata->Add(new PerfdataValue("influxdbwriter_" + influxdbwriter->GetName() + "_work_queue_items", workQueueItems));
		perfdata->Add(new PerfdataValue("influxdbwriter_" + influxdbwriter->GetName() + "_work_queue_item_rate", workQueueItemRate));
		perfdata->Add(new PerfdataValue("influxdbwriter_" + influxdbwriter->GetName() + "_data_queue_items", dataBufferItems));
	}

	status->Set("influxdbwriter", new Dictionary(std::move(nodes)));
}

boost::beast::http::request<boost::beast::http::string_body> InfluxdbWriter::AssembleRequest(String body)
{
	auto request (ObjectImpl<InfluxdbWriter>::AssembleRequest(std::move(body)));
	Dictionary::Ptr basicAuth = GetBasicAuth();

	if (basicAuth) {
		request.set(
			boost::beast::http::field::authorization,
			"Basic " + Base64::Encode(basicAuth->Get("username") + ":" + basicAuth->Get("password"))
		);
	}

	return std::move(request);
}

Url::Ptr InfluxdbWriter::AssembleUrl()
{
	auto url (ObjectImpl<InfluxdbWriter>::AssembleUrl());

	std::vector<String> path;
	path.emplace_back("write");
	url->SetPath(path);

	url->AddQueryElement("db", GetDatabase());

	if (!GetUsername().IsEmpty())
		url->AddQueryElement("u", GetUsername());
	if (!GetPassword().IsEmpty())
		url->AddQueryElement("p", GetPassword());

	return std::move(url);
}
