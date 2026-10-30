#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// A Model Context Protocol server over stdio, tools only, on nlohmann-json (no official C++ SDK).
// Newline-delimited JSON-RPC 2.0 on the inherited stdin/stdout pipes; logs on stderr only.

// Versions: legacy 2024-11-05..2025-11-25 (initialize handshake, what clients send today) and
// modern 2026-07-28 (stateless _meta envelope, server/discover), both from one process.

// Not offered: resources, prompts, logging, sampling, tasks, JSON-RPC batches.
namespace Mcp
{
    using Json = nlohmann::json;

    // A tool's own failure: answered as a result with isError, its message as is.
    struct ToolError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // Thrown by a handler that noticed its request was cancelled: no response is sent.
    struct Cancelled
    {
    };

    // Fired when the client cancels a request (or closes stdin). The work in progress registers a
    // hook (terminating the child it started) run once on Fire, or at once if already fired.
    class CancelToken
    {
    public:
        void Fire();
        bool Fired() const { return Flag.load(); }
        void OnFire(std::function<void()> NewHook);
        void Clear();

    private:
        std::atomic<bool> Flag{ false };
        std::mutex Lock;
        std::function<void()> Hook;
    };

    // What a tool handler gets besides its arguments.
    class Call
    {
    public:
        Call(CancelToken& C, Json ProgressToken, std::function<void(const Json&)> Send)
            : Cancel(C), Token(std::move(ProgressToken)), Send(std::move(Send)) {}

        CancelToken& Cancel;
        // notifications/progress; a no-op without a progressToken. Only a value above the last
        // one sent goes out: the spec wants progress to increase with each notification.
        void Progress(double Value, std::optional<double> Total, const std::string& Message);

    private:
        Json Token;
        std::function<void(const Json&)> Send;
        double Last = -1;
    };

    struct ToolResult
    {
        Json Content = Json::array();
        Json Structured;   // null: none
        bool IsError = false;

        // An object: as structuredContent, and as JSON text for clients that read content only.
        static ToolResult Object(const Json& Value);
        static ToolResult Image(const std::vector<uint8_t>& Bytes, const std::string& MimeType);
        static ToolResult Error(const std::string& Message);
    };

    struct Tool
    {
        std::string Name, Title, Description;
        Json InputSchema;    // JSON Schema object
        Json OutputSchema;   // null: none (a tool answering an image)
        Json Annotations;    // readOnlyHint, destructiveHint, idempotentHint, openWorldHint
        std::function<ToolResult(const Json& Args, Call& C)> Run;
    };

    struct ServerInfo
    {
        std::string Name, Title, Version, Instructions;
    };

    // Serves until stdin closes or breaks. Then every request in flight is cancelled and
    // OnShutdown stops what the tools started. Returns an exit code.
    int Serve(const ServerInfo& Info, std::vector<Tool> Tools, std::function<void()> OnShutdown);

    // A line on stderr (the client's log of this server) and in the app log.
    void Log(const std::string& Line);
}
