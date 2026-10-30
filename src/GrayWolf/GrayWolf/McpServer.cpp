// NOMINMAX before <windows.h>: std::max/std::min below would be mangled by its macros.
#define NOMINMAX
#include <Windows.h>

#include "McpServer.h"

#include "AppLog.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <io.h>
#include <map>
#include <memory>
#include <thread>

namespace Mcp
{
namespace
{
    constexpr const char* kModern = "2026-07-28";
    // Newest first: the legacy handshake answers the client's version when it is one of these.
    const char* const kLegacy[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };
    constexpr const char* kVersionKey = "io.modelcontextprotocol/protocolVersion";
    constexpr const char* kServerInfoKey = "io.modelcontextprotocol/serverInfo";

    // JSON-RPC and MCP error codes
    constexpr int kParseError = -32700, kInvalidRequest = -32600, kMethodNotFound = -32601,
                  kInvalidParams = -32602, kUnsupportedVersion = -32022;

    // Never throws on bad UTF-8 (a report or a path can hold some): replaced, not fatal.
    std::string Dump(const Json& J)
    {
        return J.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    Json SupportedVersions()
    {
        Json V = Json::array({ kModern });
        for (const char* L : kLegacy) V.push_back(L);
        return V;
    }

    // Light check of the arguments against a tool's inputSchema: required keys, and the JSON type
    // of each known property (null stands for "not given"). Enough for the flat schemas used here.
    std::string CheckArguments(const Json& Schema, const Json& Args)
    {
        if (!Args.is_object()) return "arguments: an object is expected";
        for (const auto& R : Schema.value("required", Json::array()))
            if (!Args.contains(R.get<std::string>()) || Args[R.get<std::string>()].is_null())
                return std::format("{}: required", R.get<std::string>());
        const Json Props = Schema.value("properties", Json::object());
        for (const auto& [Key, Value] : Args.items())
        {
            if (!Props.contains(Key) || Value.is_null()) continue;
            const Json& P = Props[Key];
            Json Types = P.value("type", Json());
            if (Types.is_string()) Types = Json::array({ Types });
            if (!Types.is_array()) continue;
            auto Is = [&](const std::string& T, const Json& V) {
                if (T == "string") return V.is_string();
                if (T == "integer") return V.is_number_integer() || (V.is_number_float() && V.get<double>() == (double)(long long)V.get<double>());
                if (T == "number") return V.is_number();
                if (T == "boolean") return V.is_boolean();
                if (T == "array") return V.is_array();
                if (T == "object") return V.is_object();
                return T == "null" && V.is_null();
            };
            bool Ok = false;
            for (const auto& T : Types) Ok = Ok || Is(T.get<std::string>(), Value);
            if (!Ok) return std::format("{}: expected {}", Key, Dump(P["type"]));
            if (Value.is_array() && P.contains("items") && P["items"].value("type", "") == "string")
                for (const auto& Item : Value)
                    if (!Item.is_string()) return std::format("{}: a list of strings is expected", Key);
        }
        return {};
    }

    class Server
    {
    public:
        Server(const ServerInfo& I, std::vector<Tool> T) : Info(I), Tools(std::move(T)) {}

        int Run(std::function<void()> OnShutdown);

    private:
        struct InFlight
        {
            CancelToken Cancel;
        };

        ServerInfo Info;
        std::vector<Tool> Tools;
        HANDLE In = INVALID_HANDLE_VALUE, Out = INVALID_HANDLE_VALUE;
        std::mutex WriteLock;
        std::mutex FlightLock;
        std::map<std::string, std::shared_ptr<InFlight>> Flights;   // by the request id's JSON
        std::mutex LiveLock;
        std::condition_variable LiveDone;
        int Live = 0;                                               // tools/call threads running

        void Send(const Json& Message);
        void Reply(const Json& Id, Json Result, bool Modern);
        void Fail(const Json& Id, int Code, const std::string& Message, const Json& Data = Json());
        void Handle(const std::string& Line);
        void HandleRequest(const Json& Id, const std::string& Method, const Json& Params);
        void CallTool(Json Id, Json Params, bool Modern);
        Json ServerInfoJson() const;
        Json ToolList() const;
    };

    void Server::Send(const Json& Message)
    {
        const std::string Line = Dump(Message) + "\n";
        std::lock_guard<std::mutex> G(WriteLock);
        const char* P = Line.data();
        size_t Left = Line.size();
        while (Left > 0)
        {
            DWORD Wrote = 0;
            if (!WriteFile(Out, P, (DWORD)std::min<size_t>(Left, 1 << 20), &Wrote, nullptr) || Wrote == 0) return;   // client gone
            P += Wrote;
            Left -= Wrote;
        }
    }

    Json Server::ServerInfoJson() const
    {
        return { { "name", Info.Name }, { "title", Info.Title }, { "version", Info.Version } };
    }

    // Modern results carry resultType and the server's identity; legacy ones stay as they were.
    void Server::Reply(const Json& Id, Json Result, bool Modern)
    {
        if (Modern)
        {
            Result["resultType"] = "complete";
            Result["_meta"][kServerInfoKey] = ServerInfoJson();
        }
        Send({ { "jsonrpc", "2.0" }, { "id", Id }, { "result", std::move(Result) } });
    }

    void Server::Fail(const Json& Id, int Code, const std::string& Message, const Json& Data)
    {
        Json Error = { { "code", Code }, { "message", Message } };
        if (!Data.is_null()) Error["data"] = Data;
        Send({ { "jsonrpc", "2.0" }, { "id", Id }, { "error", std::move(Error) } });
    }

    Json Server::ToolList() const
    {
        Json List = Json::array();
        for (const Tool& T : Tools)
        {
            Json J = { { "name", T.Name }, { "title", T.Title }, { "description", T.Description },
                       { "inputSchema", T.InputSchema }, { "annotations", T.Annotations } };
            J["annotations"]["title"] = T.Title;
            if (!T.OutputSchema.is_null()) J["outputSchema"] = T.OutputSchema;
            List.push_back(std::move(J));
        }
        return List;
    }

    void Server::Handle(const std::string& Line)
    {
        Json Message = Json::parse(Line, nullptr, false);
        if (Message.is_discarded()) return Fail(nullptr, kParseError, "Parse error");
        if (Message.is_array()) return Fail(nullptr, kInvalidRequest, "Invalid Request: JSON-RPC batches are not supported");
        if (!Message.is_object()) return Fail(nullptr, kInvalidRequest, "Invalid Request");
        const Json Id = Message.contains("id") ? Message["id"] : Json();
        if (!Message.contains("method"))
            return;   // a response to a request of ours: this server sends none
        if (Message.value("jsonrpc", "") != "2.0" || !Message["method"].is_string() ||
            (Message.contains("id") && !Id.is_string() && !Id.is_number_integer()))
            return Fail(Id.is_string() || Id.is_number_integer() ? Id : Json(), kInvalidRequest, "Invalid Request");
        const std::string Method = Message["method"];
        const Json Params = Message.contains("params") && Message["params"].is_object() ? Message["params"] : Json::object();

        if (!Message.contains("id"))
        {
            // notifications: only a cancellation needs anything done
            if (Method == "notifications/cancelled" && Params.contains("requestId"))
            {
                std::shared_ptr<InFlight> F;
                {
                    std::lock_guard<std::mutex> G(FlightLock);
                    auto It = Flights.find(Dump(Params["requestId"]));
                    if (It != Flights.end()) F = It->second;
                }
                if (F)
                {
                    Log(std::format("request {} cancelled by the client", Dump(Params["requestId"])));
                    F->Cancel.Fire();
                }
            }
            return;
        }
        HandleRequest(Id, Method, Params);
    }

    void Server::HandleRequest(const Json& Id, const std::string& Method, const Json& Params)
    {
        // A request carrying the modern envelope is served statelessly at that version.
        const Json Meta = Params.contains("_meta") && Params["_meta"].is_object() ? Params["_meta"] : Json::object();
        const bool Modern = Meta.contains(kVersionKey);
        if (Modern && Meta[kVersionKey] != kModern)
            return Fail(Id, kUnsupportedVersion, "Unsupported protocol version",
                        { { "supported", SupportedVersions() }, { "requested", Meta[kVersionKey] } });

        if (Method == "initialize")
        {
            std::string Version = kLegacy[0];
            const std::string Asked = Params.value("protocolVersion", "");
            for (const char* L : kLegacy)
                if (Asked == L) Version = L;
            Log(std::format("initialize: client {} asked {}, answered {}",
                            Params.contains("clientInfo") ? Dump(Params["clientInfo"]) : "?", Asked, Version));
            return Reply(Id, { { "protocolVersion", Version },
                               { "capabilities", { { "tools", { { "listChanged", false } } } } },
                               { "serverInfo", ServerInfoJson() },
                               { "instructions", Info.Instructions } }, false);
        }
        if (Method == "server/discover")
            return Reply(Id, { { "supportedVersions", SupportedVersions() },
                               { "capabilities", { { "tools", { { "listChanged", false } } } } },
                               { "instructions", Info.Instructions },
                               { "ttlMs", 0 }, { "cacheScope", "private" } }, true);
        if (Method == "ping")
            return Reply(Id, Json::object(), Modern);
        if (Method == "tools/list")
        {
            Json Result = { { "tools", ToolList() } };
            // the list never changes while this process runs
            if (Modern) { Result["ttlMs"] = 3600000; Result["cacheScope"] = "private"; }
            return Reply(Id, std::move(Result), Modern);
        }
        if (Method == "tools/call")
        {
            if (!Params.contains("name") || !Params["name"].is_string())
                return Fail(Id, kInvalidParams, "tools/call: name missing");
            // its own thread: the reading loop stays free for ping, status and cancellations
            {
                std::lock_guard<std::mutex> G(LiveLock);
                Live++;
            }
            std::thread([this, Id, Params, Modern] {
                CallTool(Id, Params, Modern);
                std::lock_guard<std::mutex> G(LiveLock);
                if (--Live == 0) LiveDone.notify_all();
            }).detach();
            return;
        }
        Fail(Id, kMethodNotFound, "Method not found: " + Method);
    }

    void Server::CallTool(Json Id, Json Params, bool Modern)
    {
        const std::string Name = Params["name"];
        const Tool* T = nullptr;
        for (const Tool& Candidate : Tools)
            if (Candidate.Name == Name) T = &Candidate;
        if (T == nullptr) return Fail(Id, kInvalidParams, "Unknown tool: " + Name);
        const Json Args = Params.contains("arguments") && !Params["arguments"].is_null() ? Params["arguments"] : Json::object();

        auto Flight = std::make_shared<InFlight>();
        const std::string Key = Dump(Id);
        {
            std::lock_guard<std::mutex> G(FlightLock);
            Flights[Key] = Flight;
        }
        Json Token;
        if (Params.contains("_meta") && Params["_meta"].is_object() && Params["_meta"].contains("progressToken"))
            Token = Params["_meta"]["progressToken"];
        Call C(Flight->Cancel, Token, [this](const Json& M) { Send(M); });

        ToolResult R;
        bool Silent = false;
        const auto Start = std::chrono::steady_clock::now();
        const std::string Bad = CheckArguments(T->InputSchema, Args);
        if (!Bad.empty())
            R = ToolResult::Error("Invalid arguments: " + Bad);
        else
        {
            try { R = T->Run(Args, C); }
            catch (const Cancelled&) { Silent = true; }
            catch (const ToolError& E) { R = ToolResult::Error(E.what()); }
            catch (const std::exception& E) { R = ToolResult::Error(std::string("internal error: ") + E.what()); }
        }
        {
            std::lock_guard<std::mutex> G(FlightLock);
            Flights.erase(Key);
        }
        // A cancelled request gets no response (the spec's SHOULD NOT).
        Silent = Silent || Flight->Cancel.Fired();
        Log(std::format("{} {} in {} ms", Name, Silent ? "cancelled" : R.IsError ? "failed" : "ok",
                        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Start).count()));
        if (Silent) return;
        Json Result = { { "content", R.Content } };
        if (!R.Structured.is_null()) Result["structuredContent"] = R.Structured;
        if (R.IsError) Result["isError"] = true;
        Reply(Id, std::move(Result), Modern);
    }

    int Server::Run(std::function<void()> OnShutdown)
    {
        // Private, non-inheritable copies of the pipes; the std handles (CRT and Win32) then point
        // at NUL, so nothing else in the process, nor any child, can write into the protocol.
        HANDLE Self = GetCurrentProcess();
        DuplicateHandle(Self, GetStdHandle(STD_INPUT_HANDLE), Self, &In, 0, FALSE, DUPLICATE_SAME_ACCESS);
        DuplicateHandle(Self, GetStdHandle(STD_OUTPUT_HANDLE), Self, &Out, 0, FALSE, DUPLICATE_SAME_ACCESS);
        FILE* F = nullptr;
        if (freopen_s(&F, "NUL", "r", stdin) != 0 || freopen_s(&F, "NUL", "w", stdout) != 0)
            Log("mcp: could not point stdin/stdout at NUL");
        SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdin)));
        SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
        if (In == nullptr || In == INVALID_HANDLE_VALUE || Out == nullptr || Out == INVALID_HANDLE_VALUE)
        {
            Log("mcp: no stdin/stdout pipes (run it from an MCP client: claude mcp add graywolf -- <path>\\GrayWolf.exe mcp)");
            return 2;
        }
        Log(std::format("mcp: serving {} tools ({} {})", Tools.size(), Info.Name, Info.Version));

        std::string Buffer;
        char Chunk[65536];
        for (;;)
        {
            DWORD Read = 0;
            if (!ReadFile(In, Chunk, sizeof(Chunk), &Read, nullptr) || Read == 0) break;   // EOF or broken pipe
            Buffer.append(Chunk, Read);
            size_t Start = 0, End;
            while ((End = Buffer.find('\n', Start)) != std::string::npos)
            {
                std::string Line = Buffer.substr(Start, End - Start);
                Start = End + 1;
                if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                if (Line.find_first_not_of(" \t") == std::string::npos) continue;
                Handle(Line);
            }
            Buffer.erase(0, Start);
        }

        // stdin closed: the client is gone. Cancel what runs, stop the children, then wait a bit
        // for the request threads to see it.
        Log("mcp: stdin closed, shutting down");
        {
            std::lock_guard<std::mutex> G(FlightLock);
            for (auto& [Key, Flight] : Flights) Flight->Cancel.Fire();
        }
        if (OnShutdown) OnShutdown();
        std::unique_lock<std::mutex> G(LiveLock);
        const bool Drained = LiveDone.wait_for(G, std::chrono::seconds(10), [this] { return Live == 0; });
        G.unlock();
        if (!Drained)
        {
            // a thread stuck past its children's end: leave without running static destructors under it
            Log("mcp: request threads still running, exiting anyway");
            ExitProcess(0);
        }
        CloseHandle(In);
        CloseHandle(Out);
        return 0;
    }
}

void CancelToken::Fire()
{
    std::lock_guard<std::mutex> G(Lock);
    Flag = true;
    if (Hook) Hook();
}

void CancelToken::OnFire(std::function<void()> NewHook)
{
    std::lock_guard<std::mutex> G(Lock);
    Hook = std::move(NewHook);
    if (Flag && Hook) Hook();
}

void CancelToken::Clear()
{
    std::lock_guard<std::mutex> G(Lock);
    Hook = nullptr;
}

void Call::Progress(double Value, std::optional<double> Total, const std::string& Message)
{
    if (Token.is_null() || Value <= Last) return;
    Last = Value;
    Json Params = { { "progressToken", Token }, { "progress", Value }, { "message", Message } };
    if (Total) Params["total"] = *Total;
    Send({ { "jsonrpc", "2.0" }, { "method", "notifications/progress" }, { "params", std::move(Params) } });
}

ToolResult ToolResult::Object(const Json& Value)
{
    ToolResult R;
    R.Structured = Value;
    R.Content.push_back({ { "type", "text" }, { "text", Dump(Value) } });
    return R;
}

ToolResult ToolResult::Image(const std::vector<uint8_t>& Bytes, const std::string& MimeType)
{
    static const char* Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string B64;
    B64.reserve((Bytes.size() + 2) / 3 * 4);
    for (size_t I = 0; I < Bytes.size(); I += 3)
    {
        const uint32_t N = (uint32_t)Bytes[I] << 16 | (I + 1 < Bytes.size() ? (uint32_t)Bytes[I + 1] << 8 : 0) |
                           (I + 2 < Bytes.size() ? (uint32_t)Bytes[I + 2] : 0);
        B64 += Alphabet[(N >> 18) & 63];
        B64 += Alphabet[(N >> 12) & 63];
        B64 += I + 1 < Bytes.size() ? Alphabet[(N >> 6) & 63] : '=';
        B64 += I + 2 < Bytes.size() ? Alphabet[N & 63] : '=';
    }
    ToolResult R;
    R.Content.push_back({ { "type", "image" }, { "data", std::move(B64) }, { "mimeType", MimeType } });
    return R;
}

ToolResult ToolResult::Error(const std::string& Message)
{
    ToolResult R;
    R.IsError = true;
    R.Content.push_back({ { "type", "text" }, { "text", Message } });
    return R;
}

int Serve(const ServerInfo& Info, std::vector<Tool> Tools, std::function<void()> OnShutdown)
{
    Server S(Info, std::move(Tools));
    return S.Run(std::move(OnShutdown));
}

void Log(const std::string& Line)
{
    AppLog::Log().info("{}", Line);
    const HANDLE Err = GetStdHandle(STD_ERROR_HANDLE);
    if (Err == nullptr || Err == INVALID_HANDLE_VALUE) return;
    const std::string Text = Line + "\n";
    DWORD Wrote = 0;
    WriteFile(Err, Text.data(), (DWORD)Text.size(), &Wrote, nullptr);
}
}
