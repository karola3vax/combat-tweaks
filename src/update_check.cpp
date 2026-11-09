#include "update_check.h"

#include "version_compare.h"

#include <ISmmPlugin.h>
#include <steam/isteamhttp.h>
#include <steam/steam_api_common.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <vector>

PLUGIN_GLOBALVARS();

namespace update_check
{
namespace
{

using Clock = std::chrono::steady_clock;

constexpr const char *kUrl = "https://api.github.com/repos/karola3vax/combat-tweaks/releases/latest";
constexpr auto kInterval = std::chrono::hours(6);
constexpr auto kSteamRetry = std::chrono::seconds(30); // Steam's HTTP interface is not up yet
constexpr uint32 kTimeoutSeconds = 10;
constexpr uint32 kMaxReplyBytes = 1024 * 1024; // release notes and asset lists make the reply a few KB to a few hundred

void Completed(HTTPRequestCompleted_t *result, bool io_failure);

// One request at a time. Static, so its address stays fixed while Steam holds the call result.
struct Request
{
	HTTPRequestHandle handle = INVALID_HTTPREQUEST_HANDLE;
	CCallResult<Request, HTTPRequestCompleted_t> call;
	void Done(HTTPRequestCompleted_t *result, bool io_failure) { Completed(result, io_failure); }
} g_request;

bool g_enabled = true;
bool g_started = false;
std::string g_current;
std::string g_announced; // the last version printed, so each one prints once
Clock::time_point g_next_check {};

void Release()
{
	g_request.call.Cancel();
	if (g_request.handle == INVALID_HTTPREQUEST_HANDLE) return;
	if (ISteamHTTP *http = SteamGameServerHTTP()) http->ReleaseHTTPRequest(g_request.handle);
	g_request.handle = INVALID_HTTPREQUEST_HANDLE;
}

void Send()
{
	ISteamHTTP *http = SteamGameServerHTTP();
	if (!http)
	{
		g_next_check = Clock::now() + kSteamRetry;
		return;
	}
	g_next_check = Clock::now() + kInterval; // 404, rate limits and network errors all wait for the next round
	const HTTPRequestHandle handle = http->CreateHTTPRequest(k_EHTTPMethodGET, kUrl);
	SteamAPICall_t call = k_uAPICallInvalid;
	if (handle == INVALID_HTTPREQUEST_HANDLE
		|| !http->SetHTTPRequestHeaderValue(handle, "Accept", "application/vnd.github+json")
		// Steam always sends its own User-Agent; if it refuses the replacement, append ours to it.
		|| !(http->SetHTTPRequestHeaderValue(handle, "User-Agent", "combat-tweaks-update-check")
			|| http->SetHTTPRequestUserAgentInfo(handle, "combat-tweaks-update-check"))
		|| !http->SetHTTPRequestRequiresVerifiedCertificate(handle, true)
		|| !http->SetHTTPRequestNetworkActivityTimeout(handle, kTimeoutSeconds)
		|| !http->SetHTTPRequestAbsoluteTimeoutMS(handle, kTimeoutSeconds * 1000)
		|| !http->SendHTTPRequest(handle, &call) || call == k_uAPICallInvalid)
	{
		if (handle != INVALID_HTTPREQUEST_HANDLE) http->ReleaseHTTPRequest(handle);
		return;
	}
	g_request.handle = handle;
	g_request.call.SetGameserverFlag();
	g_request.call.Set(call, &g_request, &Request::Done);
}

void Completed(HTTPRequestCompleted_t *result, bool io_failure)
{
	ISteamHTTP *http = SteamGameServerHTTP();
	const int status = result && !io_failure && result->m_bRequestSuccessful ? static_cast<int>(result->m_eStatusCode) : 0;
	std::vector<char> body;
	if (http && status == 200 && result->m_unBodySize > 0 && result->m_unBodySize <= kMaxReplyBytes)
	{
		body.resize(result->m_unBodySize);
		if (!http->GetHTTPResponseBodyData(g_request.handle, reinterpret_cast<uint8 *>(body.data()), result->m_unBodySize))
			body.clear();
	}
	if (http) http->ReleaseHTTPRequest(g_request.handle);
	g_request.handle = INVALID_HTTPREQUEST_HANDLE;
	if (body.empty()) return;

	const nlohmann::json reply = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
	if (!reply.is_object()) return;
	const auto tag = reply.find("tag_name");
	if (tag == reply.end() || !tag->is_string()) return;
	std::string latest = tag->get<std::string>();
	if (!Newer(latest, g_current)) return; // also rejects tags that are not plain versions
	if (latest.front() == 'v' || latest.front() == 'V') latest.erase(0, 1);
	if (latest == g_announced) return;
	g_announced = latest;
	META_CONPRINTF("[COMBAT_TWEAKS] Combat Tweaks %s is out (this server runs %s): "
				   "https://github.com/karola3vax/combat-tweaks/releases/latest\n",
		latest.c_str(), g_current.c_str());
}

} // namespace

void Enabled(bool enabled)
{
	g_enabled = enabled;
	if (!enabled) Release();
}

void Start(const char *version)
{
	g_current = version;
	g_started = true;
	g_next_check = Clock::now();
}

void Frame()
{
	if (!g_enabled || !g_started || g_request.handle != INVALID_HTTPREQUEST_HANDLE || Clock::now() < g_next_check) return;
	Send();
}

void Stop()
{
	Release();
	g_started = false;
}

} // namespace update_check
