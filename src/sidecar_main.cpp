#include "codex_client.hpp"
#include "cdp_client.hpp"
#include "session_stats.hpp"
#include "desktop_launch.hpp"
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <thread>

namespace {
std::atomic_bool interrupted{false};
BOOL WINAPI console_signal(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT){interrupted=true;return TRUE;}
    return FALSE;
}
int64_t now_seconds() {return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::wstring env(const wchar_t* name) {
    DWORD length=GetEnvironmentVariableW(name,nullptr,0);if(!length)return {};
    std::wstring result(length,L'\0');DWORD written=GetEnvironmentVariableW(name,result.data(),length);result.resize(written<length?written:0);return result;
}
std::string narrow(const std::wstring& value) {
    int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(length,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length,nullptr,nullptr);return result;
}
Json token_json(const PulseTokenUsage& usage) {
    if(!usage.present)return nullptr;
    return {{"inputTokens",usage.input_tokens},{"cachedInputTokens",usage.cached_input_tokens},
            {"cacheWriteInputTokens",usage.cache_write_input_tokens},{"outputTokens",usage.output_tokens},
            {"reasoningOutputTokens",usage.reasoning_output_tokens},{"totalTokens",usage.total_tokens}};
}
Json usage_json(const SessionStats& stats) {
    const auto& u=stats.usage;
    return {{"total",token_json(u.total)},{"last",token_json(u.last)},
            {"contextWindow",u.model_context_window>0?Json(u.model_context_window):Json(nullptr)},
            {"timestamp",u.timestamp},{"outputTokensPerSecond",stats.output_tokens_per_second>=0?Json(stats.output_tokens_per_second):Json(nullptr)},
            {"error",narrow(stats.error)},{"tailLimited",stats.tail_limited}};
}
Json window_json(const PulseWindow& value) {
    if(!value.present)return nullptr;
    return {{"usedPercent",value.used_percent},{"remainingPercent",pulse_remaining(&value)},
            {"windowDurationMins",value.duration_mins<0?Json(nullptr):Json(value.duration_mins)},
            {"resetsAt",value.resets_at<0?Json(nullptr):Json(value.resets_at)}};
}
Json quota_json(const FetchResult& result,int64_t updated) {
    Json pools=Json::array();
    for(size_t i=0;i<result.snapshot.pool_count;++i) {
        const auto& p=result.snapshot.pools[i];
        pools.push_back({{"id",p.id},{"name",p.name},{"plan",p.plan},
                        {"primary",window_json(p.primary)},{"secondary",window_json(p.secondary)},
                        {"ordinaryUsageAllowed",p.ordinary_usage_allowed<0?Json(nullptr):Json(p.ordinary_usage_allowed!=0)},
                        {"credits",p.has_credits?Json{{"unlimited",p.unlimited_credits!=0},{"balance",p.credit_balance}}:Json(nullptr)}});
    }
    return {{"pools",pools},{"updatedAt",updated<0?Json(nullptr):Json(updated)},
            {"error",narrow(result.error)},{"truncated",result.snapshot.truncated!=0},
            {"resetCredits",result.reset_credit_count<0?Json(nullptr):Json(result.reset_credit_count)},
            {"resetEligible",reset_allowed(result)}};
}
uint16_t port_value(const std::wstring& value) {
    if(value.empty() || value.size()>5)return 0;
    unsigned port=0;for(wchar_t digit:value){if(digit<L'0'||digit>L'9')return 0;port=port*10+digit-L'0';}
    return port>0 && port<=65535?static_cast<uint16_t>(port):0;
}
uint16_t profile_port(const std::filesystem::path& profile) {
    std::ifstream file(profile/L"DevToolsActivePort");std::string line;
    if(!std::getline(file,line) || line.size()>16)return 0;
    if(!line.empty()&&line.back()=='\r')line.pop_back();
    return port_value(utf8_to_wide(line.c_str()));
}
std::filesystem::path executable_dir() {
    std::wstring path(32768,L'\0');DWORD length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));path.resize(length);
    return std::filesystem::path(path).parent_path();
}
std::string read_widget(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Widget file not found. Keep widget/inline.js beside the executable.");
    std::string text((std::istreambuf_iterator<char>(file)),{});
    if(text.empty() || text.size()>512*1024)throw std::runtime_error("Widget source size is invalid.");
    return text;
}
std::string conversation_id(const std::filesystem::path& path) {
    static const std::regex uuid("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    std::string name=narrow(path.stem().wstring());std::smatch match;
    if(!std::regex_search(name,match,uuid))return {};
    std::string result=match.str();for(auto& ch:result)ch=static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));return result;
}
bool allowed_target(const Json& target,bool preview) {
    if(target.value("type","")!="page")return false;
    const std::string url=target.value("url",""), title=target.value("title","");
    if(preview)return title=="Codex Dashboard Fixture" && (url.starts_with("file://")||url.starts_with("http://127.0.0.1:"));
    // The installed renderer uses app://-. app://fs is a file viewer, not a composer.
    for(const auto* page:{"app://-/index.html","app://-/detached-window.html"}) {
        std::string prefix=page;
        if(url==prefix || url.starts_with(prefix+"?") || url.starts_with(prefix+"#"))return true;
    }
    return false;
}
struct Target {std::string websocket;std::unique_ptr<CdpClient> client;SessionReader reader;};
void help() {
    std::cout<<"Codex Dashboard 0.1.0 - native C23/C++23 collector + inline renderer\n"
             <<"  --attach                  Attach beside Codex's chat composer (default)\n"
             <<"  --cdp-port <1..65535>      Use an existing loopback debugger\n"
             <<"  --profile <directory>     Read DevToolsActivePort from this profile\n"
             <<"  --stop                    Remove the widget and stop this sidecar\n"
             <<"  --status                  Verify live widgets and debugger port\n"
             <<"  --launch                  Activate the registered desktop with debug flags\n"
             <<"  --once                    Read live quotas as sanitized JSON and exit\n"
             <<"  --session <rollout.jsonl>  Also read this explicit session\n"
             <<"  --codex <codex.exe>        Use a specific native Codex CLI\n"
             <<"  --codex-home <directory>   Session storage (CLI auth remains inherited)\n"
             <<"  --widget <inline.js>       Widget source path\n"
             <<"  --preview                 Only attach to the isolated preview fixture\n";
}
}

int wmain(int argc,wchar_t** argv) {
    SetConsoleCtrlHandler(console_signal,TRUE);
    std::wstring cli,home;
    std::filesystem::path session,profile=std::filesystem::path(env(L"APPDATA"))/L"Codex"/L"web"/L"Codex";
    std::filesystem::path widget=executable_dir()/L"widget"/L"inline.js";
    bool once=false,stop=false,preview=false,status=false,launch=false;uint16_t port=0;
    for(int i=1;i<argc;++i) {
        std::wstring arg=argv[i];
        if(arg==L"--help"||arg==L"-h"){help();return 0;}
        if(arg==L"--once")once=true;
        else if(arg==L"--status")status=true;
        else if(arg==L"--launch")launch=true;
        else if(arg==L"--stop")stop=true;
        else if(arg==L"--preview")preview=true;
        else if(arg==L"--attach"){}
        else if(arg==L"--codex"||arg==L"--session"||arg==L"--profile"||arg==L"--cdp-port"||arg==L"--widget"||arg==L"--codex-home") {
            if(++i>=argc){help();return 2;}
            if(arg==L"--codex")cli=argv[i];else if(arg==L"--session")session=argv[i];else if(arg==L"--profile")profile=argv[i];
            else if(arg==L"--widget")widget=argv[i];else if(arg==L"--codex-home")home=argv[i];
            else {port=port_value(argv[i]);if(!port){std::cerr<<"Invalid debugger port.\n";return 2;}}
        } else {help();return 2;}
    }
    try {
        if(launch){std::cout<<Json{{"activated",true},{"pid",launch_codex_desktop()}}.dump()<<'\n';return 0;}
        CodexClient codex(cli);
        if(once) {
            auto result=codex.fetch(interrupted);Json output{{"quotas",quota_json(result,result.ok?now_seconds():-1)}};
            if(!session.empty()){SessionReader reader;output["usage"]=usage_json(reader.read(session));}
            std::cout<<output.dump(2)<<'\n';return result.ok?0:1;
        }
        if(!port)port=profile_port(profile);
        if(!port){std::cerr<<"Codex renderer debugging is not enabled. No application was changed or restarted.\n"
                                   <<"After finishing active chats, close Codex and use Start-Codex-Dashboard.ps1.\n";return 4;}
        if(status) {
            Json widgets=Json::array();
            for(const auto& page:cdp_targets(port))if(allowed_target(page,preview)) {
                CdpClient client;
                if(!client.connect(page.value("webSocketDebuggerUrl","")))continue;
                auto reply=client.command("Runtime.evaluate",{{"expression","window.__codexDashboard?.state()||{mounted:false}"},{"returnByValue",true}});
                auto value=reply.at("result").value("value",Json::object());
                widgets.push_back(Json{{"mounted",value.value("mounted",false)}});
            }
            std::cout<<Json{{"port",port},{"debugReady",true},{"widgets",widgets}}.dump()<<'\n';return 0;
        }
        std::wstring event_name=L"Local\\CodexDashboardStop_"+std::to_wstring(port);
        if(stop) {
            HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,event_name.c_str());
            if(!event){std::cout<<"No dashboard sidecar is running on this port.\n";return 0;}
            SetEvent(event);CloseHandle(event);return 0;
        }
        std::wstring mutex_name=L"Local\\CodexDashboard_"+std::to_wstring(port);
        HANDLE mutex=CreateMutexW(nullptr,FALSE,mutex_name.c_str());
        if(!mutex)throw std::runtime_error("Cannot create the sidecar ownership lock.");
        if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);std::cout<<"Dashboard is already running.\n";return 0;}
        HANDLE event=CreateEventW(nullptr,TRUE,FALSE,event_name.c_str());
        if(!event){CloseHandle(mutex);throw std::runtime_error("Cannot create the sidecar stop event.");}
        std::string source=read_widget(widget);
        std::jthread cancellation([event](std::stop_token token) {
            while(!token.stop_requested())if(WaitForSingleObject(event,100)==WAIT_OBJECT_0){interrupted=true;break;}
        });
        std::vector<Target> targets;
        FetchResult quotas;int64_t quota_time=-1;ULONGLONG queried=0,discovered=0;
        auto sessions=list_sessions(home);
        std::cout<<"Waiting for a Codex composer on loopback port "<<port<<". Ctrl+C or --stop removes the widget.\n";
        while(!interrupted && WaitForSingleObject(event,0)==WAIT_TIMEOUT) {
            try {
                if(GetTickCount64()-discovered>5000 || targets.empty()) {
                    discovered=GetTickCount64();auto pages=cdp_targets(port);
                    std::vector<std::string> active;
                    if(!pages.is_array())throw std::runtime_error("Invalid debugger target list.");
                    for(const auto& page:pages) {
                        if(!allowed_target(page,preview))continue;
                        std::string socket=page.value("webSocketDebuggerUrl","");active.push_back(socket);
                        if(std::any_of(targets.begin(),targets.end(),[&](const Target& t){return t.websocket==socket;}))continue;
                        if(targets.size()>=8)break;
                        auto client=std::make_unique<CdpClient>();
                        if(!client->connect(socket))continue;
                        client->command("Runtime.evaluate",{{"expression",source},{"returnByValue",true}});
                        targets.push_back({socket,std::move(client),SessionReader{}});
                    }
                    std::erase_if(targets,[&](const Target& t){return std::find(active.begin(),active.end(),t.websocket)==active.end();});
                }
                for(auto it=targets.begin();it!=targets.end();) {
                    try {
                        auto state_result=it->client->command("Runtime.evaluate",{{"expression","({...window.__codexDashboard?.state(), initialized: Boolean(window.__codexDashboard), visible: !document.hidden})"},{"returnByValue",true}});
                        Json state=state_result.at("result").value("value",Json::object());
                        if(!state.value("initialized",false)) {
                            it->client->command("Runtime.evaluate",{{"expression",source}});
                            ++it;continue;
                        }
                        if(!state.value("mounted",false)){++it;continue;}
                        Json reset_result;
                        if(state.contains("resetRequest") && state["resetRequest"].is_object() && state["resetRequest"].value("confirmed",false)) {
                            std::string key=state["resetRequest"].value("id",std::string{});
                            reset_result={{"id",key},{"outcome",codex.reset(key,interrupted)}};
                            auto refreshed=codex.fetch(interrupted);
                            if(refreshed.ok){quotas=std::move(refreshed);quota_time=now_seconds();}
                            queried=GetTickCount64();
                        }
                        if(GetTickCount64()-queried>60000 || !queried) {
                            queried=GetTickCount64();auto result=codex.fetch(interrupted);
                            if(result.ok){quotas=std::move(result);quota_time=now_seconds();}
                            else {quotas.ok=false;quotas.error=std::move(result.error);}
                            sessions=list_sessions(home);
                        }
                        std::string id;
                        if(state.contains("conversationId")&&state["conversationId"].is_string())id=state["conversationId"].get<std::string>();
                        SessionStats stats;stats.usage.model_context_window=-1;
                        auto file=session;
                        if(file.empty()&&!id.empty())for(const auto& entry:sessions)if(conversation_id(entry.path)==id){file=entry.path;break;}
                        if(!file.empty() && !id.empty() && conversation_id(file)==id)stats=it->reader.read(file);
                        else stats.error=L"此会话未找到本地 Token 快照。";
                        Json payload{{"conversationId",id.empty()?Json(nullptr):Json(id)},{"usage",usage_json(stats)},{"quotas",quota_json(quotas,quota_time)}};
                        if(!reset_result.is_null())payload["resetResult"]=reset_result;
                        it->client->command("Runtime.evaluate",{{"expression","window.__codexDashboard?.update("+payload.dump()+")"}});
                        ++it;
                    } catch(const std::exception&) {it=targets.erase(it);}
                }
            } catch(const std::exception&) {
                // No raw debugger errors, page contents, or account fields are logged.
            }
            for(int i=0;i<20 && !interrupted && WaitForSingleObject(event,0)==WAIT_TIMEOUT;++i)Sleep(100);
        }
        for(auto& target:targets)try{target.client->command("Runtime.evaluate",{{"expression","window.__codexDashboard?.dispose()"}});}catch(const std::exception&){}
        codex.close();cancellation.request_stop();cancellation.join();CloseHandle(event);CloseHandle(mutex);std::cout<<"Dashboard detached.\n";return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
