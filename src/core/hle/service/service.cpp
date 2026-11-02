// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <fmt/ranges.h>
#include <string_view>
#include <thread>
#include "common/assert.h"
#include "common/kledd_slow_log.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/reporter.h"

namespace Service {

/// @brief Creates a function string for logging, complete with the name (or header code, depending
/// on what's passed in) the port name, and all the cmd_buf arguments.
[[maybe_unused]] static std::string MakeFunctionString(std::string_view name, std::string_view port_name, const u32* cmd_buf) {
    // Number of params == bits 0-5 + bits 6-11
    int num_params = (cmd_buf[0] & 0x3F) + ((cmd_buf[0] >> 6) & 0x3F);
    std::string function_string = fmt::format("fn '{}': port={}", name, port_name);
    for (int i = 1; i <= num_params; ++i)
        function_string += fmt::format(", cmd_buf[{}]={:#x}", i, cmd_buf[i]);
    return function_string;
}

ServiceFrameworkBase::ServiceFrameworkBase(Core::System& system_, const char* service_name_, u32 max_sessions_, InvokerFn* handler_invoker_)
    : SessionRequestHandler(system_.Kernel(), service_name_)
    , system{system_}
    , service_name{service_name_}
    , is_i_storage{std::string_view{service_name_} == "IStorage"}
    , handler_invoker{handler_invoker_}
    , max_sessions{max_sessions_}
{}

ServiceFrameworkBase::~ServiceFrameworkBase() {
    // Wait for other threads to release access before destroying
    const auto guard = ServiceFrameworkBase::LockService();
}

void ServiceFrameworkBase::RegisterHandlersBase(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers.reserve(handlers.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers.emplace_hint(handlers.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::RegisterHandlersBaseTipc(const FunctionInfoBase* functions, std::size_t n) {
    // Usually this array is sorted by id already, so hint to insert at the end
    handlers_tipc.reserve(handlers_tipc.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        handlers_tipc.emplace_hint(handlers_tipc.cend(), functions[i].expected_header, functions[i]);
}

void ServiceFrameworkBase::ReportUnimplementedFunction(HLERequestContext& ctx,
                                                       const FunctionInfoBase* info) {
    auto cmd_buf = ctx.CommandBuffer();
    std::string function_name = info == nullptr ? "<unknown>" : info->name;

    fmt::memory_buffer buf;
    fmt::format_to(std::back_inserter(buf), "function '{}({})': port='{}' cmd_buf={{[0]={:#x}", ctx.GetCommand(), function_name, service_name, cmd_buf[0]);
    for (int i = 1; i <= 8; ++i)
        fmt::format_to(std::back_inserter(buf), ", [{}]={:#x}", i, cmd_buf[i]);
    buf.push_back('}');

    system.GetReporter().SaveUnimplementedFunctionReport(ctx, ctx.GetCommand(), function_name, service_name);
    UNIMPLEMENTED_MSG("Unknown / unimplemented {}", fmt::to_string(buf));
    if (Settings::values.use_auto_stub) {
        LOG_WARNING(Service, "Using auto stub fallback!");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }
}

void ServiceFrameworkBase::InvokeRequest(HLERequestContext& ctx) {
    const auto command = ctx.GetCommand();
    auto it = handlers.find(command);
    const bool is_cmd_read = command == 0;
    FunctionInfoBase const* info = it == handlers.end() ? nullptr : &it->second;
    if (info == nullptr || info->handler_callback == nullptr)
        return ReportUnimplementedFunction(ctx, info);

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    const auto handler_start{std::chrono::steady_clock::now()};
    handler_invoker(this, info->handler_callback, ctx);

    // eden-kledd diagnostic: the audio services share one server thread, so one slow request
    // delays the game's audio renderer updates queued behind it.
    const std::string_view name{service_name};
    const bool audio_service{name.starts_with("IAudio") || name.starts_with("aud") ||
                             name.starts_with("IHardwareOpus") || name == "hwopus"};
    const bool fs_service{name == "IFile" || name == "IStorage" || name == "IFileSystem" ||
                          name == "IDirectory" || name == "fsp-srv"};
    if (audio_service || fs_service) {
        const auto handler_ms{std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - handler_start)
                                  .count()};
        if (handler_ms >= 20) {
            if (audio_service) {
                LOG_WARNING(Service, "{}::{} took {} ms on its service thread", name, info->name,
                            handler_ms);
            } else {
                // eden-kledd diagnostic: file-system requests are served one at a time, so a slow
                // read during a load delays every other file request queued behind it.
                static Common::Kledd::SlowEventLog slow_fs{};
                u32 n{};
                u32 worst{};
                if (slow_fs.Note(static_cast<u32>(handler_ms), n, worst)) {
                    LOG_WARNING(Service,
                                "[Kledd] file system: {} requests took over 20 ms in the last 2 s "
                                "(longest {} ms, latest {}::{})",
                                n, worst, name, info->name);
                }
            }
        }
    }

    // eden-kledd: keep upstream PR #4316's NVDEC-gated read pacing. Without it, booting
    // Metroid Prime 4 with the speed limit off crashes during the intro videos
    // (the game's media code races its own RomFS reads).
    if (is_i_storage && is_cmd_read) {
        const auto* const process = ctx.GetThread().GetOwnerProcess();
        if (process != nullptr && system.IsNVDECActiveForProcess(process->GetId())) {
            std::this_thread::sleep_for(std::chrono::microseconds{600});
        }
    }
}

void ServiceFrameworkBase::InvokeRequestTipc(HLERequestContext& ctx) {
    auto it = handlers_tipc.find(ctx.GetCommand());
    FunctionInfoBase const* info = it == handlers_tipc.end() ? nullptr : &it->second;
    if (info == nullptr || info->handler_callback == nullptr)
        return ReportUnimplementedFunction(ctx, info);

    LOG_TRACE(Service, "{}", MakeFunctionString(info->name, GetServiceName(), ctx.CommandBuffer()));
    handler_invoker(this, info->handler_callback, ctx);
}

Result ServiceFrameworkBase::HandleSyncRequest(Kernel::KServerSession& session,
                                               HLERequestContext& ctx) {
    const auto guard = LockService();

    Result result = ResultSuccess;

    switch (ctx.GetCommandType()) {
    case IPC::CommandType::Close:
    case IPC::CommandType::TIPC_Close: {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
        result = IPC::ResultSessionClosed;
        break;
    }
    case IPC::CommandType::ControlWithContext:
    case IPC::CommandType::Control: {
        system.ServiceManager().InvokeControlRequest(ctx);
        break;
    }
    case IPC::CommandType::RequestWithContext:
    case IPC::CommandType::Request: {
        InvokeRequest(ctx);
        break;
    }
    default:
        if (ctx.IsTipc()) {
            InvokeRequestTipc(ctx);
            break;
        }

        UNIMPLEMENTED_MSG("command_type={}", ctx.GetCommandType());
        break;
    }

    // If emulation was shutdown, we are closing service threads, do not write the response back to
    // memory that may be shutting down as well.
    if (system.IsPoweredOn()) {
        ctx.WriteToOutgoingCommandBuffer();
    }

    return result;
}

} // namespace Service
