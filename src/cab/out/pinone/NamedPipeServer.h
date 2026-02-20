#pragma once

#include <string>
#include <atomic>
#include <thread>

#include <libserialport.h>

namespace DOF
{

class NamedPipeServer
{
private:
   std::atomic<bool> m_isRunning { true };
   std::string m_comPort;
   std::string m_pipeName;
   int m_baudRate = 2000000;
   int m_rawFd = -1; // Fallback for when libserialport fails
   static std::atomic<int> s_instanceCount;
   bool m_isDuplicate = false;
   std::thread m_serverThread;
   struct sp_port* m_serialPort = nullptr;
#ifndef _WIN32
   int m_serverSocket = -1;
#endif

   void HandleClientConnection(void* serverStream);

public:
   NamedPipeServer(const std::string& pipeName, const std::string& comPort, int baudRate = 2000000);
   ~NamedPipeServer();

   void StartServer();
   void StopServer();
};

}