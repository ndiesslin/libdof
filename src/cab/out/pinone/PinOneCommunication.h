#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace DOF
{

class NamedPipeServer;

class PinOneCommunication
{
private:
   void* m_pipeClient = nullptr;
   NamedPipeServer* m_server = nullptr;
   std::string m_pipeName;
   std::string m_comPort;
   int m_baudRate = 2000000;


public:
   PinOneCommunication(const std::string& comPort, int baudRate = 2000000);
   ~PinOneCommunication();

   bool ConnectToServer();
   bool DisconnectFromServer();
   bool CreateServer();
   bool IsComPortConnected();
   void Disconnect();
   void Write(const std::vector<uint8_t>& bytesToWrite);
   std::string ReadLine();
   std::string GetCOMPort();

private:
   void SendPipeMessage(const std::string& message);
   std::string ReadMessage();
};

}