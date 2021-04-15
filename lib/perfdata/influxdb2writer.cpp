/* Icinga 2 | (c) 2021 Icinga GmbH | GPLv2+ */

#include "perfdata/influxdb2writer.hpp"
#include "perfdata/influxdb2writer-ti.cpp"
#include "remote/url.hpp"
#include "base/configtype.hpp"
#include "base/perfdatavalue.hpp"
#include "base/statsfunction.hpp"
#include <utility>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

using namespace icinga;

REGISTER_TYPE(Influxdb2Writer);

REGISTER_STATSFUNCTION(Influxdb2Writer, &Influxdb2Writer::StatsFunc);

void Influxdb2Writer::StatsFunc(const Dictionary::Ptr& status, const Array::Ptr& perfdata)
{
	DictionaryData nodes;

	for (const Influxdb2Writer::Ptr& influxdbwriter : ConfigType::GetObjectsByType<Influxdb2Writer>()) {
		size_t workQueueItems = influxdbwriter->m_WorkQueue.GetLength();
		double workQueueItemRate = influxdbwriter->m_WorkQueue.GetTaskCount(60) / 60.0;
		size_t dataBufferItems = influxdbwriter->m_DataBuffer.size();

		nodes.emplace_back(influxdbwriter->GetName(), new Dictionary({
			{ "work_queue_items", workQueueItems },
			{ "work_queue_item_rate", workQueueItemRate },
			{ "data_buffer_items", dataBufferItems }
		}));

		perfdata->Add(new PerfdataValue("influxdb2writer_" + influxdbwriter->GetName() + "_work_queue_items", workQueueItems));
		perfdata->Add(new PerfdataValue("influxdb2writer_" + influxdbwriter->GetName() + "_work_queue_item_rate", workQueueItemRate));
		perfdata->Add(new PerfdataValue("influxdb2writer_" + influxdbwriter->GetName() + "_data_queue_items", dataBufferItems));
	}

	status->Set("influxdb2writer", new Dictionary(std::move(nodes)));
}

boost::beast::http::request<boost::beast::http::string_body> Influxdb2Writer::AssembleRequest(String body)
{
	auto request (ObjectImpl<Influxdb2Writer>::AssembleRequest(std::move(body)));

	request.set(boost::beast::http::field::authorization, "Token " + GetAuthToken());

	return std::move(request);
}

Url::Ptr Influxdb2Writer::AssembleUrl()
{
	auto url (ObjectImpl<Influxdb2Writer>::AssembleUrl());

	std::vector<String> path ({"api", "v2", "write"});
	url->SetPath(path);

	url->AddQueryElement("org", GetOrganization());
	url->AddQueryElement("bucket", GetBucket());

	return std::move(url);
}
