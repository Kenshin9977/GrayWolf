#include "stdafx.h"

#include "WebClient.h"
#include "Strings.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

#pragma comment(lib, "Winhttp.lib")

const static wchar_t* UserAgent = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/67.0.3396.99 Safari/537.36 WraithX";
// The default download buffer length (10mb)
const uint32_t DownloadBufferLength = 0xA00000;
// The default timeout length (100s)
const uint32_t TimeoutLength = 10000;

// TODO: We can report download / upload progress, download if content-length is specified (Default to indeterminate)
// TODO: Should use a universal callback w/ the flags

struct UriObject
{
    INTERNET_PORT PortType;
    std::wstring HostName;
    std::wstring Path;

    UriObject()
    {
        this->PortType = INTERNET_DEFAULT_PORT;
        this->HostName = L"";
        this->Path = L"";
    }
};

const UriObject PreParseUrl(std::string Url)
{
    auto Result = UriObject();

    // Check for and remove the HTTP(s):// from the Url, we don't want it
    if (_strnicmp(Url.c_str(), "https://", strlen("https://")) == 0)
    {
        Result.PortType = INTERNET_DEFAULT_HTTPS_PORT;
        Url = Url.substr(8);
    }
    else if (_strnicmp(Url.c_str(), "http://", strlen("http://")) == 0)
    {
        Result.PortType = INTERNET_DEFAULT_HTTP_PORT;
        Url = Url.substr(7);
    }
    else if (_strnicmp(Url.c_str(), "//", strlen("//")) == 0)
    {
        Url = Url.substr(7);
    }
    
    auto PathPosition = Url.find_first_of("/");

    Result.HostName = Strings::ToUnicodeString(Url.substr(0, PathPosition));

    if (PathPosition != std::string::npos)
        Result.Path = Strings::ToUnicodeString(Url.substr(Url.find_first_of("/")));

    return Result;
}

WebClient::WebClient(ProxyType Proxy)
{
    this->InternetHandle = WinHttpOpen(UserAgent, (Proxy == ProxyType::DefaultProxy) ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, NULL);

    if (this->InternetHandle == NULL)
    {
#if _DEBUG
        throw std::runtime_error("Failed to initialize HTTP library");
#else
        throw std::runtime_error("");
#endif
    }

    uint32_t DecodeOption = WINHTTP_DECOMPRESSION_FLAG_ALL;

    WinHttpSetOption(this->InternetHandle, WINHTTP_OPTION_DECOMPRESSION, &DecodeOption, sizeof(uint32_t));
    WinHttpSetTimeouts(this->InternetHandle, TimeoutLength, TimeoutLength, TimeoutLength, TimeoutLength);
}

WebClient::~WebClient()
{
    if (this->InternetHandle != NULL)
        WinHttpCloseHandle(this->InternetHandle);
}

void WebClient::SetTimeout(uint32_t Milliseconds)
{
    WinHttpSetTimeouts(this->InternetHandle, Milliseconds, Milliseconds, Milliseconds, Milliseconds);
}

std::optional<std::vector<uint8_t>> WebClient::DownloadData(const std::string& Url, uint32_t* Status)
{
    if (Status) *Status = 0;
    auto Uri = PreParseUrl(Url);

    HINTERNET hConnect = NULL, hRequest = NULL;
    BOOL CanDownload = false;

    hConnect = WinHttpConnect(this->InternetHandle, Uri.HostName.c_str(), Uri.PortType, NULL);
    if (hConnect)
        hRequest = WinHttpOpenRequest(hConnect, L"GET", Uri.Path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, (Uri.PortType == INTERNET_DEFAULT_HTTPS_PORT) ? WINHTTP_FLAG_SECURE : NULL);
    if (hRequest)
        CanDownload = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, NULL, WINHTTP_NO_REQUEST_DATA, NULL, NULL, NULL);
    if (CanDownload)
        CanDownload = WinHttpReceiveResponse(hRequest, NULL);

    if (CanDownload && Status)
    {
        DWORD Code = 0, CodeSize = sizeof(Code);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Code, &CodeSize, WINHTTP_NO_HEADER_INDEX))
            *Status = Code;
    }

    if (CanDownload)
    {
        DWORD BufferSize = 0, Downloaded = 0;

        std::vector<uint8_t> Result;

        do
        {
            BufferSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &BufferSize))
                break;

            if (!BufferSize)
                break;

            auto TempBuffer = std::make_unique<char[]>(std::min<uint32_t>(BufferSize, DownloadBufferLength));
            WinHttpReadData(hRequest, (LPVOID)TempBuffer.get(), std::min<uint32_t>(BufferSize, DownloadBufferLength), &Downloaded);

            Result.insert(Result.end(), TempBuffer.get(), TempBuffer.get() + Downloaded);
        } while (BufferSize > 0);

        if (hRequest)
            WinHttpCloseHandle(hRequest);
        if (hConnect)
            WinHttpCloseHandle(hConnect);

        return Result;
    }

    if (hRequest)
        WinHttpCloseHandle(hRequest);
    if (hConnect)
        WinHttpCloseHandle(hConnect);

    return std::nullopt;
}
