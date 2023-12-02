///
/// Created by Anonymous275 on 12/26/21
/// Copyright (c) 2021-present Anonymous275 read the LICENSE file for more info.
///

#define WIN32_LEAN_AND_MEAN
#include "Launcher.h"
#include <ShlObj.h>
#include <comutil.h>
#include <shellapi.h>
#include <windows.h>
#include <csignal>
#include <mutex>
#include "HttpAPI.h"
#include "Logger.h"
#include "Memory/Memory.h"

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* p) {
   LOG(ERROR) << "CAUGHT EXCEPTION! Code 0x" << std::hex << std::uppercase
              << p->ExceptionRecord->ExceptionCode;
   return EXCEPTION_EXECUTE_HANDLER;
}

Launcher::Launcher(int argc, char* argv[]) :
    CurrentPath(fs::current_path()),
    DiscordMessage("Just launched") {
   try {
      Log::Init();
      Shutdown.store(false);
      Exit.store(false);
      Launcher::StaticAbort(this);
      DiscordTime = std::time(nullptr);
      WindowsInit();
      SetUnhandledExceptionFilter(CrashHandler);
      LOG(INFO) << "Starting Launcher v" << FullVersion;
      BackendProxy = std::thread(&Launcher::StartProxy, this);

      fs::path config_path(CurrentPath / "Launcher.toml");

      std::string arg, arg2;
      if(argc > 2) {
         arg = argv[1];
         arg2 = argv[2];
      } else if (argc > 1) {
         arg = argv[1];
      }
      if (arg.starts_with('0')) {
         LOG(INFO) << "Debug param in effect";
         DebugMode = true;
         Memory::DebugMode = true;
      } else if (arg2.starts_with("beammp://")) {
         CurrentPath = arg;
         config_path = CurrentPath / "Launcher.toml";
         if (arg2.starts_with("beammp://connect/")) {
            ConnectURI = arg2.substr(17);
         }
      } else if (!arg.empty()) {
         config_path = CurrentPath / arg;
      }

      LoadConfig(config_path);
      LauncherCache = CurrentPath/"Resources";
   } catch (const ShutdownException& e) {
      Abort();
      throw ShutdownException(e.what());
   }
}

void Launcher::Abort() {
   Shutdown.store(true);
   ServerHandler.Close();
   if (DiscordRPC.joinable()) {
      DiscordRPC.join();
   }
   if (IPCSystem.joinable()) {
      IPCSystem.join();
   }
   HTTPProxy.stop();
   if (BackendProxy.joinable()) {
      BackendProxy.join();
   }
   if (!MPUserPath.empty()) {
      ResetMods();
   }
   if (GamePID != 0) {
      auto Handle = OpenProcess(PROCESS_TERMINATE, false, DWORD(GamePID));
      TerminateProcess(Handle, 0);
      CloseHandle(Handle);
   }
}

Launcher::~Launcher() {
   if (!Shutdown.load()) {
      Abort();
   }
}

void ShutdownHandler(int sig) {
   Launcher::StaticAbort();
   while (HTTP::isDownload) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
   }
   LOG(INFO) << "Got termination signal (" << sig << ")";
   while (!Launcher::getExit()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
   }
}

void Launcher::StaticAbort(Launcher* Instance) {
   static Launcher* Address;
   if (Instance) {
      Address = Instance;
      return;
   }
   Address->Abort();
}

void Launcher::WindowsInit() {
    system("cls");
    SetConsoleTitleA(("BeamMP Launcher v" + FullVersion).c_str());
    signal(SIGINT, ShutdownHandler);
    signal(SIGTERM, ShutdownHandler);
    signal(SIGABRT, ShutdownHandler);
    signal(SIGBREAK, ShutdownHandler);


    std::wstring command = L"\"" + (CurrentPath/"BeamMP-Launcher.exe").wstring() + L"\" \"" + CurrentPath.wstring() + L"\" \"%1\"";
    std::wstring ICON = L"\"" + (CurrentPath/"BeamMP-Launcher.exe").wstring() + L"\",0";
    std::wstring URL = L"URL:beammp Protocol";
    HKEY hKey;
    RegCreateKey(HKEY_CLASSES_ROOT, L"beammp", &hKey);
    RegSetValueEx(hKey, nullptr, 0, REG_SZ, (BYTE*)URL.c_str(), URL.size()*2);
    RegSetValueEx(hKey, L"URL Protocol", 0, REG_SZ, (BYTE*)"", 0);

    RegCreateKey(HKEY_CLASSES_ROOT, L"beammp\\DefaultIcon", &hKey);
    RegSetValueEx(hKey, nullptr, 0, REG_SZ, (BYTE*)ICON.c_str(), ICON.size()*2);

    RegCreateKey(HKEY_CLASSES_ROOT, L"beammp\\shell\\open\\command", &hKey);
    RegSetValueEx(hKey, nullptr, 0, REG_SZ, (BYTE*)command.c_str(), command.size()*2);
    RegCloseKey(hKey);

}

void Launcher::LaunchGame() {
   /*VersionParser GameVersion(BeamVersion);
   if (GameVersion.data[1] > SupportedVersion.data[1]) {
      LOG(FATAL) << "BeamNG V" << BeamVersion
                 << " not yet supported, please wait until we update BeamMP!";
      throw ShutdownException("Fatal Error");
   } else if (GameVersion.data[1] < SupportedVersion.data[1]) {
      LOG(FATAL) << "BeamNG V" << BeamVersion
                 << " not supported, please update and launch the new update!";
      throw ShutdownException("Fatal Error");
   } else if (GameVersion > SupportedVersion) {
      LOG(WARNING)
          << "BeamNG V" << BeamVersion
          << " is slightly newer than recommended, this might cause issues!";
   } else if (GameVersion < SupportedVersion) {
      LOG(WARNING)
          << "BeamNG V" << BeamVersion
          << " is slightly older than recommended, this might cause issues!";
   }*/



   if (Memory::GetBeamNGPID({}) == 0) {
      if(Memory::GetLauncherPID({GetCurrentProcessId()}) != 0) {
         Abort();
         LOG(ERROR) << "Two launchers are open with no game process";
         return;
      }
      LOG(INFO) << "Launching BeamNG from steam";
      ShellExecuteA(nullptr, nullptr, "steam://rungameid/284160", nullptr,
                    nullptr, SW_SHOWNORMAL);
      // ShowWindow(GetConsoleWindow(), HIDE_WINDOW);
   }
   LOG(INFO) << "Waiting for a game process, please start BeamNG manually in "
                "case of steam issues";
}

void Launcher::WaitForGame() {
   std::set<uint32_t> BlackList;
   int chan = 1;
   while (GamePID == 0 && !Shutdown.load()) {
      auto PID = Memory::GetBeamNGPID(BlackList);
      if (PID != 0 && IPC::mem_used(PID)) {
         BlackList.emplace(PID);
         LOG(INFO) << "Skipping Channel #" << chan++;
      } else {
         GamePID = PID;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
   }
   if (Shutdown.load()) return;

   if (GamePID == 0) {
      LOG(FATAL) << "Game process not found! aborting";
      throw ShutdownException("Fatal Error");
   }

   LOG(INFO) << "Game found! PID " << GamePID;

   IPCToGame   = std::make_unique<IPC>(GamePID, 0x1900000);
   IPCFromGame = std::make_unique<IPC>(GamePID + 1, 0x1900000);

   IPCSystem = std::thread(&Launcher::ListenIPC, this);
   Memory::Inject(GamePID);
   setDiscordMessage("In menus");
   while (!Shutdown.load() && Memory::GetBeamNGPID(BlackList) != 0) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
   }
   LOG(INFO) << "Game process was lost";
   GamePID = 0;
}

void Launcher::StartProxy() {
   HTTPProxy.Get("/:any", [](const httplib::Request& req, httplib::Response& res) {
      httplib::Client cli("https://backend.beammp.com");
      if (auto cli_res = cli.Get(req.path); cli_res) {
         res.set_content(cli_res->body,cli_res->get_header_value("Content-Type"));
      } else {
         res.set_content(to_string(cli_res.error()), "text/plain");
      }
   });

   HTTPProxy.Post("/:any", [](const httplib::Request& req, httplib::Response& res) {
      httplib::Client cli("https://backend.beammp.com");
      if (auto cli_res = cli.Post(req.path, req.body, req.get_header_value("Content-Type")); cli_res) {
         res.set_content(cli_res->body,cli_res->get_header_value("Content-Type"));
      } else {
        res.set_content(to_string(cli_res.error()), "text/plain");
      }
   });

   ProxyPort = HTTPProxy.bind_to_any_port("0.0.0.0");
   LOG(INFO) << "HTTP Proxy running on port " << ProxyPort;
   HTTPProxy.listen_after_bind();
}

void Launcher::ListenIPC() {
   while (!Shutdown.load()) {
      IPCFromGame->receive();
      if (!IPCFromGame->receive_timed_out()) {
         auto& MSG = IPCFromGame->msg();
         if (MSG[0] == 'C') {
            HandleIPC(IPCFromGame->msg().substr(1));
         } else {
            ServerHandler.ServerSend(IPCFromGame->msg().substr(1), false);
         }
         IPCFromGame->confirm_receive();
      }
   }
}

void Launcher::SendIPC(const std::string& Data, bool core) {
   static std::mutex Lock;
   std::scoped_lock Guard(Lock);
   if (core) IPCToGame->send("C" + Data);
   else IPCToGame->send("G" + Data);
   if (IPCToGame->send_timed_out()) {
      LOG(WARNING) << "Timed out while sending \"" << Data << "\"";
   }
}

std::string Launcher::QueryValue(HKEY& hKey, const char* Name) {
   DWORD keySize;
   BYTE buffer[16384];
   if (RegQueryValueExA(hKey, Name, nullptr, nullptr, buffer, &keySize) ==
       ERROR_SUCCESS) {
      return {(char*)buffer, keySize - 1};
   }
   return {};
}

const std::string& Launcher::getFullVersion() {
   return FullVersion;
}

const std::string& Launcher::getProtocolVersion() {
   return ProtocolVersion;
}

const std::string& Launcher::getVersion() {
   return Version;
}

const std::string& Launcher::getUserRole() {
   return UserRole;
}

bool Launcher::Terminated() noexcept {
   return Shutdown.load();
}

bool Launcher::getExit() noexcept {
   return Exit.load();
}

void Launcher::setExit(bool exit) noexcept {
   Exit.store(exit);
}

const fs::path& Launcher::getMPUserPath() {
   return MPUserPath;
}

const std::string& Launcher::getPublicKey() {
   return PublicKey;
}

const fs::path& Launcher::getCachePath() {
   if (!fs::exists(LauncherCache)) {
      fs::create_directories(LauncherCache);
   }
   return LauncherCache;
}