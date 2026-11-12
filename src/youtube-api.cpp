#include "youtube-api.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/bmem.h>

#include <json.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#define YT_TAG "[obs-multi-rtmp][youtube] "

using json = nlohmann::json;

namespace youtube {

static const char* kScope = "https://www.googleapis.com/auth/youtube";

// ---------------------------------------------------------------------------
// Auth storage
// ---------------------------------------------------------------------------

static std::filesystem::path AuthFilePath()
{
    char* p = obs_module_config_path("youtube-auth.json");
    std::string s = p ? p : "";
    bfree(p);
    return std::filesystem::u8path(s);
}

YouTubeAuthConfig LoadAuth()
{
    YouTubeAuthConfig auth;
    try {
        std::ifstream f(AuthFilePath(), std::ios::binary);
        if (!f)
            return auth;
        auto j = json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.is_object())
            return auth;
        auth.clientId = j.value("client-id", "");
        auth.clientSecret = j.value("client-secret", "");
        auth.refreshToken = j.value("refresh-token", "");
        auth.channelTitle = j.value("channel-title", "");
    } catch (...) {
    }
    return auth;
}

void SaveAuth(const YouTubeAuthConfig& auth)
{
    try {
        auto path = AuthFilePath();
        std::filesystem::create_directories(path.parent_path());
        json j;
        j["client-id"] = auth.clientId;
        j["client-secret"] = auth.clientSecret;
        j["refresh-token"] = auth.refreshToken;
        j["channel-title"] = auth.channelTitle;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << j.dump(2);
    } catch (const std::exception& e) {
        blog(LOG_ERROR, YT_TAG "Failed to save auth: %s", e.what());
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string Form(std::initializer_list<std::pair<const char*, std::string>> fields)
{
    std::string out;
    for (auto& f : fields) {
        if (!out.empty())
            out += '&';
        out += f.first;
        out += '=';
        out += UrlEncode(f.second);
    }
    return out;
}

static json ParseJson(const std::string& body)
{
    auto j = json::parse(body, nullptr, false);
    if (j.is_discarded())
        return json::object();
    return j;
}

// Pulls a human-readable message out of a Google error response.
static std::string GoogleError(const json& j, int status)
{
    try {
        if (j.contains("error")) {
            auto& e = j["error"];
            if (e.is_object()) {
                std::string msg = e.value("message", "");
                if (e.contains("errors") && e["errors"].is_array() && !e["errors"].empty()) {
                    std::string reason = e["errors"][0].value("reason", "");
                    if (!reason.empty())
                        msg += " (" + reason + ")";
                }
                if (!msg.empty())
                    return msg;
            } else if (e.is_string()) {
                std::string msg = e.get<std::string>();
                std::string desc = j.value("error_description", "");
                return desc.empty() ? msg : msg + ": " + desc;
            }
        }
    } catch (...) {
    }
    return "HTTP " + std::to_string(status);
}

static std::string NowIso8601()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static std::string Trim(std::string s)
{
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    auto end = s.find_last_not_of(ws);
    if (end == std::string::npos)
        return "";
    s.erase(end + 1);
    return s;
}

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error; // transport error
};

} // namespace youtube

// ---------------------------------------------------------------------------
// HTTP transport (WinHTTP on Windows; unsupported elsewhere)
// ---------------------------------------------------------------------------

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace youtube {

bool IsSupported() { return true; }

static std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static HttpResponse Http(const std::string& method, const std::string& url,
    const std::vector<std::string>& headers, const std::string& body)
{
    HttpResponse res;
    std::wstring wurl = Widen(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        res.error = "Bad URL";
        return res;
    }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    HINTERNET session = WinHttpOpen(L"obs-multi-rtmp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"obs-multi-rtmp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        res.error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")";
        return res;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);

    HINTERNET conn = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    HINTERNET req = nullptr;
    if (conn) {
        DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        req = WinHttpOpenRequest(conn, Widen(method).c_str(), path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    }

    if (req) {
        std::string allHeaders;
        for (auto& h : headers)
            allHeaders += h + "\r\n";
        std::wstring wh = Widen(allHeaders);

        BOOL ok = WinHttpSendRequest(req,
            wh.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wh.c_str(), wh.empty() ? 0 : (DWORD)-1L,
            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
            (DWORD)body.size(), 0);
        if (ok)
            ok = WinHttpReceiveResponse(req, nullptr);
        if (ok) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            res.status = (int)status;

            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
                    break;
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0)
                    break;
                res.body.append(chunk.data(), read);
            }
        } else {
            res.error = "Network error (" + std::to_string(GetLastError()) + ")";
        }
    } else {
        res.error = "Could not open connection (" + std::to_string(GetLastError()) + ")";
    }

    if (req)
        WinHttpCloseHandle(req);
    if (conn)
        WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return res;
}

} // namespace youtube

#else

namespace youtube {
bool IsSupported() { return false; }
static HttpResponse Http(const std::string&, const std::string&, const std::vector<std::string>&, const std::string&)
{
    HttpResponse res;
    res.error = "YouTube integration is only available in the Windows build.";
    return res;
}
} // namespace youtube

#endif

// ---------------------------------------------------------------------------
// OAuth + YouTube API
// ---------------------------------------------------------------------------

namespace youtube {

static const std::vector<std::string> kFormHeaders = { "Content-Type: application/x-www-form-urlencoded" };

std::optional<DeviceCode> RequestDeviceCode(const std::string& clientId, std::string& error)
{
    auto r = Http("POST", "https://oauth2.googleapis.com/device/code", kFormHeaders,
        Form({ { "client_id", clientId }, { "scope", kScope } }));
    if (!r.error.empty()) {
        error = r.error;
        return {};
    }
    auto j = ParseJson(r.body);
    if (r.status != 200) {
        error = GoogleError(j, r.status);
        return {};
    }
    DeviceCode dc;
    dc.deviceCode = j.value("device_code", "");
    dc.userCode = j.value("user_code", "");
    dc.verificationUrl = j.value("verification_url", "https://www.google.com/device");
    dc.interval = j.value("interval", 5);
    dc.expiresIn = j.value("expires_in", 1800);
    if (dc.deviceCode.empty() || dc.userCode.empty()) {
        error = "Unexpected response from Google";
        return {};
    }
    return dc;
}

static std::string FetchChannelTitle(const std::string& accessToken)
{
    auto r = Http("GET", "https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true",
        { "Authorization: Bearer " + accessToken }, "");
    if (r.status != 200)
        return "";
    auto j = ParseJson(r.body);
    try {
        if (j.contains("items") && !j["items"].empty())
            return j["items"][0]["snippet"].value("title", "");
    } catch (...) {
    }
    return "";
}

PollResult PollDeviceToken(const std::string& clientId, const std::string& clientSecret,
    const std::string& deviceCode, std::string& refreshToken, std::string& channelTitle,
    std::string& error)
{
    auto r = Http("POST", "https://oauth2.googleapis.com/token", kFormHeaders,
        Form({ { "client_id", clientId },
            { "client_secret", clientSecret },
            { "device_code", deviceCode },
            { "grant_type", "urn:ietf:params:oauth:grant-type:device_code" } }));
    if (!r.error.empty()) {
        // Transient network trouble: keep polling.
        return PollResult::Pending;
    }
    auto j = ParseJson(r.body);
    if (r.status == 200) {
        refreshToken = j.value("refresh_token", "");
        std::string access = j.value("access_token", "");
        if (refreshToken.empty()) {
            error = "Google did not return a refresh token";
            return PollResult::Failed;
        }
        channelTitle = FetchChannelTitle(access);
        return PollResult::Granted;
    }
    std::string code = j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : "";
    if (code == "authorization_pending")
        return PollResult::Pending;
    if (code == "slow_down")
        return PollResult::SlowDown;
    if (code == "access_denied")
        error = "Access was denied in the browser.";
    else if (code == "expired_token")
        error = "The sign-in code expired. Try again.";
    else
        error = GoogleError(j, r.status);
    return PollResult::Failed;
}

static std::optional<std::string> GetAccessToken(const YouTubeAuthConfig& auth, std::string& error)
{
    if (auth.clientId.empty() || auth.clientSecret.empty()) {
        error = "YouTube API isn't set up. Open Edit > YouTube > API setup.";
        return {};
    }
    if (auth.refreshToken.empty()) {
        error = "Not signed in to YouTube. Open Edit > YouTube and click Sign in.";
        return {};
    }
    auto r = Http("POST", "https://oauth2.googleapis.com/token", kFormHeaders,
        Form({ { "client_id", auth.clientId },
            { "client_secret", auth.clientSecret },
            { "refresh_token", auth.refreshToken },
            { "grant_type", "refresh_token" } }));
    if (!r.error.empty()) {
        error = r.error;
        return {};
    }
    auto j = ParseJson(r.body);
    if (r.status != 200) {
        std::string code = j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : "";
        if (code == "invalid_grant")
            error = "YouTube sign-in expired or was revoked. Sign in again (Edit > YouTube).";
        else
            error = "YouTube sign-in failed: " + GoogleError(j, r.status);
        return {};
    }
    auto token = j.value("access_token", "");
    if (token.empty()) {
        error = "Google did not return an access token";
        return {};
    }
    return token;
}

static std::optional<std::string> FindStreamIdForKey(const std::string& token, const std::string& key, std::string& error)
{
    std::string pageToken;
    for (int page = 0; page < 20; ++page) {
        std::string url = "https://www.googleapis.com/youtube/v3/liveStreams?part=id,cdn&mine=true&maxResults=50";
        if (!pageToken.empty())
            url += "&pageToken=" + UrlEncode(pageToken);
        auto r = Http("GET", url, { "Authorization: Bearer " + token }, "");
        if (!r.error.empty()) {
            error = r.error;
            return {};
        }
        auto j = ParseJson(r.body);
        if (r.status != 200) {
            error = "Couldn't list your YouTube stream keys: " + GoogleError(j, r.status);
            return {};
        }
        try {
            for (auto& item : j.value("items", json::array())) {
                std::string name = item["cdn"]["ingestionInfo"].value("streamName", "");
                if (!name.empty() && name == key)
                    return item.value("id", "");
            }
        } catch (...) {
        }
        pageToken = j.value("nextPageToken", "");
        if (pageToken.empty())
            break;
    }
    error = "This target's stream key wasn't found on the signed-in YouTube channel. "
            "Make sure the key in the Service tab is from the same channel.";
    return {};
}

static std::string ReadFileBytes(const std::string& utf8Path)
{
    std::ifstream f(std::filesystem::u8path(utf8Path), std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string ThumbnailContentType(const std::string& path)
{
    auto ext = std::filesystem::u8path(path).extension().string();
    for (auto& c : ext)
        c = (char)tolower((unsigned char)c);
    if (ext == ".png")
        return "image/png";
    return "image/jpeg";
}

BroadcastResult PrepareBroadcast(const YouTubeTargetConfig& cfg, const std::string& rawKey)
{
    BroadcastResult result;
    std::string key = Trim(rawKey);
    if (key.empty()) {
        result.error = "This target has no stream key set.";
        return result;
    }
    if (Trim(cfg.title).empty()) {
        result.error = "YouTube title is empty.";
        return result;
    }

    auto auth = LoadAuth();
    auto token = GetAccessToken(auth, result.error);
    if (!token)
        return result;
    const std::string authHeader = "Authorization: Bearer " + *token;

    auto streamId = FindStreamIdForKey(*token, key, result.error);
    if (!streamId)
        return result;

    // Create the broadcast.
    json body;
    body["snippet"]["title"] = cfg.title;
    body["snippet"]["description"] = cfg.description;
    body["snippet"]["scheduledStartTime"] = NowIso8601();
    body["status"]["privacyStatus"] = cfg.privacy.empty() ? "public" : cfg.privacy;
    body["status"]["selfDeclaredMadeForKids"] = cfg.madeForKids;
    body["contentDetails"]["enableAutoStart"] = true;
    body["contentDetails"]["enableAutoStop"] = true;

    auto r = Http("POST", "https://www.googleapis.com/youtube/v3/liveBroadcasts?part=snippet,status,contentDetails",
        { authHeader, "Content-Type: application/json; charset=UTF-8" }, body.dump());
    if (!r.error.empty()) {
        result.error = r.error;
        return result;
    }
    auto j = ParseJson(r.body);
    if (r.status != 200) {
        result.error = "Couldn't create YouTube broadcast: " + GoogleError(j, r.status);
        return result;
    }
    result.broadcastId = j.value("id", "");
    if (result.broadcastId.empty()) {
        result.error = "YouTube didn't return a broadcast id";
        return result;
    }
    blog(LOG_INFO, YT_TAG "Created broadcast %s", result.broadcastId.c_str());

    // Bind it to the stream key.
    r = Http("POST",
        "https://www.googleapis.com/youtube/v3/liveBroadcasts/bind?part=id,contentDetails&id=" + UrlEncode(result.broadcastId)
            + "&streamId=" + UrlEncode(*streamId),
        { authHeader }, "");
    if (!r.error.empty() || r.status != 200) {
        result.error = "Couldn't attach the broadcast to your stream key: "
            + (r.error.empty() ? GoogleError(ParseJson(r.body), r.status) : r.error);
        return result;
    }

    // Thumbnail (non-fatal).
    if (!Trim(cfg.thumbnailPath).empty()) {
        auto bytes = ReadFileBytes(cfg.thumbnailPath);
        if (bytes.empty()) {
            result.warning = "Thumbnail file couldn't be read, so it was skipped.";
        } else if (bytes.size() > 2 * 1024 * 1024) {
            result.warning = "Thumbnail is over YouTube's 2 MB limit, so it was skipped.";
        } else {
            r = Http("POST",
                "https://www.googleapis.com/upload/youtube/v3/thumbnails/set?uploadType=media&videoId=" + UrlEncode(result.broadcastId),
                { authHeader, "Content-Type: " + ThumbnailContentType(cfg.thumbnailPath) }, bytes);
            if (!r.error.empty() || r.status != 200) {
                result.warning = "Thumbnail upload failed: "
                    + (r.error.empty() ? GoogleError(ParseJson(r.body), r.status) : r.error);
            }
        }
        if (!result.warning.empty())
            blog(LOG_WARNING, YT_TAG "%s", result.warning.c_str());
    }

    result.ok = true;
    return result;
}

} // namespace youtube
