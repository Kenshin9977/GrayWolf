#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <Windows.h>
#include <winhttp.h>

enum class ProxyType
{
    NoProxy,
    DefaultProxy
};

class WebClient
{
private:
    HINTERNET InternetHandle;

public:
    WebClient(ProxyType Proxy = ProxyType::NoProxy);
    ~WebClient();

    void SetTimeout(uint32_t Milliseconds);

    // The body of a GET, nothing when the request fails; Status, when given, gets the HTTP status
    // (0 when no response came)
    std::optional<std::vector<uint8_t>> DownloadData(const std::string& Url, uint32_t* Status = nullptr);
};