/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "BreakingNews.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

bool FetchRemoteContent(const std::string& host, const std::string& port, const std::string& target, std::string& result)
{
    try 
    {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        tcp::socket socket(ioc);
        auto const results = resolver.resolve(host, port);
        net::connect(socket, results.begin(), results.end());

        http::request<http::string_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        http::write(socket, req);

        beast::flat_buffer buffer;
        http::response<http::dynamic_body> res;
        http::read(socket, buffer, res);

        result = beast::buffers_to_string(res.body().data());
        result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
        result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
        return true;
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("module", "Boost.Beast Error: {}", e.what());
        return false;
    }
}

bool TryReadNews(std::string& bn_Result)
{
    std::string url = sConfigMgr->GetOption<std::string>("BreakingNews.Url", "");
    if (url.empty())
    {
        LOG_ERROR("module", "BreakingNews.Url is not configured");
        return false;
    }

    // 解析协议部分（如http://或https://）
    size_t protocol_end = url.find("://");
    if (protocol_end != std::string::npos)
        url = url.substr(protocol_end + 3); // 移除协议头

    std::string host, port, target;

    // 分离主机/端口和路径
    size_t path_start = url.find('/');
    if (path_start != std::string::npos)
    {
        host = url.substr(0, path_start);
        target = url.substr(path_start);
    }
    else
    {
        host = url;
        target = "/";
    }

    // 分离主机和端口
    size_t colon_pos = host.find(':');
    if (colon_pos != std::string::npos)
    {
        port = host.substr(colon_pos + 1);
        host = host.substr(0, colon_pos);
    }
    else
        port = "80"; // 默认HTTP端口

    return FetchRemoteContent(host, port, target, bn_Result);
}

bool TryReadFile(std::string& path, std::string& bn_Result)
{
    std::ifstream bn_File(path);

    std::string bn_Buffer = "";
    bn_Result = "";

    if (!bn_File.is_open())
        return false;

    while (std::getline(bn_File, bn_Buffer))
        bn_Result = bn_Result + (bn_Buffer);

    bn_Result.erase(std::remove(bn_Result.begin(), bn_Result.end(), '\r'), bn_Result.cend());
    bn_Result.erase(std::remove(bn_Result.begin(), bn_Result.end(), '\n'), bn_Result.cend());

    return true;
}


bool TryReadNewsBAK(std::string& bn_Result)
{
    std::string path = sConfigMgr->GetOption<std::string>("BreakingNews.HtmlPath", "./Updates.html");
    bn_Title = sConfigMgr->GetOption<std::string>("BreakingNews.Title", "Breaking News");

    if (path == "")
    {
        LOG_ERROR("module", "Failed to read 'BreakingNews.HtmlPath'.");
        return false;
    }

    if (!TryReadFile(path, bn_Result))
    {
        LOG_ERROR("module", "Failed to read file '{}'.", path);
        return false;
    }

    return true;
}

std::vector<std::string> BreakingNewsServerScript::GetChunks(std::string s, uint8_t chunkSize)
{
    std::vector<std::string> chunks;

    for (uint32_t i = 0; i < s.size(); i += chunkSize)
        chunks.push_back(s.substr(i, chunkSize));

    return chunks;
}

void BreakingNewsServerScript::SendChunkedPayload(Warden* warden, WardenPayloadMgr* payloadMgr, std::string payload, uint32 chunkSize)
{
    bool verbose = sConfigMgr->GetOption<bool>("BreakingNews.Verbose", false);

    auto chunks = GetChunks(payload, chunkSize);

    if (!payloadMgr->GetPayloadById(_prePayloadId))
        payloadMgr->RegisterPayload(_prePayload, _prePayloadId);

    payloadMgr->QueuePayload(_prePayloadId);
    warden->ForceChecks();

    if (verbose)
        LOG_INFO("module", "Sent pre-payload '{}'.", _prePayload);

    for (auto const& chunk : chunks)
    {
        auto smallPayload = "wlbuf = wlbuf .. [[" + chunk + "]];";
    
        payloadMgr->RegisterPayload(smallPayload, _tmpPayloadId, true);
        payloadMgr->QueuePayload(_tmpPayloadId);
        warden->ForceChecks();

        if (verbose)
            LOG_INFO("module", "Sent mid-payload '{}'.", smallPayload);
    }

    if (!payloadMgr->GetPayloadById(_postPayloadId))
        payloadMgr->RegisterPayload(_postPayload, _postPayloadId);

    payloadMgr->QueuePayload(_postPayloadId);
    warden->ForceChecks();

    if (verbose)
        LOG_INFO("module", "Sent post-payload '{}'.", _postPayload);
}

void LoadBreakingNews()
{
    bn_Title = sConfigMgr->GetOption<std::string>("BreakingNews.Title", "Breaking News");

    if (!TryReadNews(bn_Body))
    {
        LOG_ERROR("module", "Failed to read breaking news.");
        return;
    }

    bn_Formatted = Acore::StringFormat(_midPayloadFmt, bn_Title, bn_Body);
}

bool BreakingNewsServerScript::CanPacketSend(WorldSession* session, WorldPacket const& packet)
{
    if (!bn_Enabled)
        return true;

    if (packet.GetOpcode() == SMSG_CHAR_ENUM)
    {
        WardenWin* warden = (WardenWin*)session->GetWarden();
        if (!warden)
            return true;

        // Trying to use Warden before it has initialized,
        // so we exit.
        if (!warden->IsInitialized())
            return true;

        if (bn_Formatted == "")
            return true;


        auto payloadMgr = warden->GetPayloadMgr();
        if (!payloadMgr)
            return true;

        // Just in-case there are some payloads in the queue, we don't want to send the incorrect payload.
        payloadMgr->ClearQueuedPayloads();

        // Load in the updated news into the cache.
        if (!sConfigMgr->GetOption<bool>("BreakingNews.Cache", false))
            LoadBreakingNews();

        // The client truncates warden packets to around 256 and our payload may be larger than that.
        SendChunkedPayload(warden, payloadMgr, bn_Formatted, 128);
    }

    return true;
}

void BreakingNewsWorldScript::OnAfterConfigLoad(bool /*reload*/)
{
    bn_Enabled = sConfigMgr->GetOption<bool>("BreakingNews.Enable", false);

    if (!bn_Enabled)
        return;

    LoadBreakingNews();
}

// Add all scripts in one.
void AddBreakingNewsScripts()
{
    new BreakingNewsWorldScript();
    new BreakingNewsServerScript();
}
