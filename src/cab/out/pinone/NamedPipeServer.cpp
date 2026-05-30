#include "NamedPipeServer.h"
#include "../../../Log.h"
#include "../../../general/StringExtensions.h"
#include <thread>
#include <vector>
#include <stdexcept>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <cstring>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#include <sys/ioctl.h>
#endif
#endif

namespace DOF
{

NamedPipeServer::NamedPipeServer(const std::string& pipeName, const std::string& comPort, int baudRate)
   : m_comPort(comPort)
   , m_pipeName(pipeName)
   , m_baudRate(baudRate)
{
#ifdef _WIN32
   sp_get_port_by_name(comPort.c_str(), &m_serialPort);
   if (m_serialPort)
   {
      enum sp_return open_resp = sp_open(m_serialPort, SP_MODE_READ_WRITE);
      if (open_resp != SP_OK) {
          Log::Warning(StringExtensions::Build("NamedPipeServer: Failed to open serial port {0} (Error: {1})", comPort, std::to_string((int)open_resp)));
          sp_free_port(m_serialPort);
          m_serialPort = nullptr;
      } else {
          bool configSuccess = true;
          if (sp_set_baudrate(m_serialPort, baudRate) != SP_OK) { Log::Error("NamedPipeServer: Failed to set baud rate"); configSuccess = false; }
          if (sp_set_bits(m_serialPort, 8) != SP_OK) { Log::Error("NamedPipeServer: Failed to set bits"); configSuccess = false; }
          if (sp_set_parity(m_serialPort, SP_PARITY_NONE) != SP_OK) { Log::Error("NamedPipeServer: Failed to set parity"); configSuccess = false; }
          if (sp_set_stopbits(m_serialPort, 1) != SP_OK) { Log::Error("NamedPipeServer: Failed to set stop bits"); configSuccess = false; }
          if (sp_set_rts(m_serialPort, SP_RTS_ON) != SP_OK) { Log::Error("NamedPipeServer: Failed to set RTS"); configSuccess = false; }
          if (sp_set_dtr(m_serialPort, SP_DTR_ON) != SP_OK) { Log::Error("NamedPipeServer: Failed to set DTR"); configSuccess = false; }

          if (!configSuccess)
          {
              Log::Error("NamedPipeServer: Configuration failed. Closing port.");
              sp_close(m_serialPort);
              sp_free_port(m_serialPort);
              m_serialPort = nullptr;
          }
      }
   }
   else {
       Log::Error(StringExtensions::Build("NamedPipeServer: libserialport could not find port {0}", comPort));
   }
#else
    // On macOS/Linux, defer serial port init to POSIX lazy open
#endif
}

NamedPipeServer::~NamedPipeServer()
{
   StopServer();
   if (m_serialPort)
   {
      sp_close(m_serialPort);
      sp_free_port(m_serialPort);
      m_serialPort = nullptr;
   }
   CloseRawSerialPort();
}

void NamedPipeServer::StartServer()
{
   m_serverThread = std::thread(
      [this]()
      {
         while (m_isRunning)
         {
#ifdef _WIN32
            std::string pipePath = "\\\\.\\pipe\\" + m_pipeName;
            HANDLE serverPipe = CreateNamedPipeA(pipePath.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 1024, 1024, 0, nullptr);

            if (serverPipe == INVALID_HANDLE_VALUE)
            {
               continue;
            }

            if (ConnectNamedPipe(serverPipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
            {
               HandleClientConnection(serverPipe);
            }

            CloseHandle(serverPipe);
#else
            m_serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
            if (m_serverSocket < 0)
            {
               continue;
            }

            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            std::string sockPath = "/tmp/" + m_pipeName;
            strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

            unlink(sockPath.c_str());

            if (bind(m_serverSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            {
               close(m_serverSocket);
               m_serverSocket = -1;
               std::this_thread::sleep_for(std::chrono::milliseconds(1000));
               continue;
            }

            if (listen(m_serverSocket, 1) < 0)
            {
               close(m_serverSocket);
               m_serverSocket = -1;
               std::this_thread::sleep_for(std::chrono::milliseconds(1000));
               continue;
            }

            int clientSock = accept(m_serverSocket, nullptr, nullptr);
            if (clientSock >= 0)
            {
               HandleClientConnection(reinterpret_cast<void*>(static_cast<intptr_t>(clientSock)));
               close(clientSock);
            }

            close(m_serverSocket);
            m_serverSocket = -1;
            unlink(sockPath.c_str());
#endif
         }
      });
}

void NamedPipeServer::HandleClientConnection(void* serverStream)
{
   bool completed = false;

   while (m_isRunning && !completed)
   {
      try
      {
         std::vector<char> request(1024);
         int bytesRead = 0;

#ifdef _WIN32
         HANDLE pipe = static_cast<HANDLE>(serverStream);
         DWORD dwBytesRead;
         if (!ReadFile(pipe, request.data(), static_cast<DWORD>(request.size()), &dwBytesRead, nullptr))
         {
            break;
         }
         bytesRead = static_cast<int>(dwBytesRead);
#else
         int sock = static_cast<int>(reinterpret_cast<intptr_t>(serverStream));

         fd_set readfds;
         FD_ZERO(&readfds);
         FD_SET(sock, &readfds);
         
         int maxFd = sock;
         if (m_serverSocket != -1)
         {
             FD_SET(m_serverSocket, &readfds);
             maxFd = std::max(maxFd, m_serverSocket);
         }

         struct timeval tv;
         tv.tv_sec = 2;
         tv.tv_usec = 0;

         int retval = select(maxFd + 1, &readfds, NULL, NULL, &tv);
         if (retval == -1) {
             break;
         } else if (retval == 0) {
             continue; // Timed out, loop and try reading again
         }

         // New connection detected - drop current client to accept new one
         if (m_serverSocket != -1 && FD_ISSET(m_serverSocket, &readfds))
         {
             break;
         }

         if (!FD_ISSET(sock, &readfds))
         {
             continue;
         }

         bytesRead = static_cast<int>(read(sock, request.data(), request.size()));
         if (bytesRead <= 0)
         {
            break;
         }
#endif

         std::string requestStr(request.data(), bytesRead);

         if (StringExtensions::StartsWith(requestStr, "CONNECT"))
         {
            if (m_serialPort && sp_get_port_handle(m_serialPort, nullptr) == SP_OK)
            {
               sp_open(m_serialPort, SP_MODE_READ_WRITE);
            }
            std::string response = "OK";
#ifdef _WIN32
            HANDLE pipe = static_cast<HANDLE>(serverStream);
            DWORD bytesWritten;
            WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.length()), &bytesWritten, nullptr);
#else
            int sock = static_cast<int>(reinterpret_cast<intptr_t>(serverStream));
            ssize_t bytesWritten = write(sock, response.c_str(), response.length());
            (void)bytesWritten;
#endif
         }
         else if (StringExtensions::StartsWith(requestStr, "STOP_SERVER"))
         {
            m_isRunning = false;
         }
         else if (StringExtensions::StartsWith(requestStr, "DISCONNECT"))
         {
            completed = true;
         }
         else if (StringExtensions::StartsWith(requestStr, "WRITE"))
         {
            std::string base64Data = requestStr.substr(6);
            std::vector<uint8_t> bytesToWrite = StringExtensions::FromBase64(base64Data);

            // Lazy recovery: If port is null, try to open it now
            if (!m_serialPort)
            {
#ifdef _WIN32
               sp_get_port_by_name(m_comPort.c_str(), &m_serialPort);
               if (m_serialPort)
               {
                   enum sp_return open_ret = sp_open(m_serialPort, SP_MODE_READ_WRITE);
                   if (open_ret == SP_OK)
                   {
                       bool configSuccess = true;
                       if (sp_set_baudrate(m_serialPort, m_baudRate) != SP_OK) configSuccess = false;
                       if (sp_set_bits(m_serialPort, 8) != SP_OK) configSuccess = false;
                       if (sp_set_parity(m_serialPort, SP_PARITY_NONE) != SP_OK) configSuccess = false;
                       if (sp_set_stopbits(m_serialPort, 1) != SP_OK) configSuccess = false;
                       if (sp_set_rts(m_serialPort, SP_RTS_ON) != SP_OK) configSuccess = false;
                       if (sp_set_dtr(m_serialPort, SP_DTR_ON) != SP_OK) configSuccess = false;
                       
                       if (!configSuccess) {
                           sp_close(m_serialPort);
                           sp_free_port(m_serialPort);
                           m_serialPort = nullptr;
                       }
                   }
                   else {
                       sp_free_port(m_serialPort);
                       m_serialPort = nullptr;
                   }
               }
#else
               OpenRawSerialPort();
#endif
            }

            if (m_serialPort)
            {
               int written = sp_blocking_write(m_serialPort, bytesToWrite.data(), bytesToWrite.size(), 500);

               if (written < 0)
               {
                   sp_close(m_serialPort);
                   sp_free_port(m_serialPort);
                   m_serialPort = nullptr;
                   
                   std::this_thread::sleep_for(std::chrono::milliseconds(200));
                   
                   sp_get_port_by_name(m_comPort.c_str(), &m_serialPort);
                   enum sp_return open_resp = SP_ERR_ARG;
                   if (m_serialPort)
                   {
                       open_resp = sp_open(m_serialPort, SP_MODE_READ_WRITE);
                   }
                   
                   if (open_resp == SP_OK)
                   {
                       bool configSuccess = true;
                       if (sp_set_baudrate(m_serialPort, m_baudRate) != SP_OK) configSuccess = false;
                       if (sp_set_bits(m_serialPort, 8) != SP_OK) configSuccess = false;
                       if (sp_set_parity(m_serialPort, SP_PARITY_NONE) != SP_OK) configSuccess = false;
                       if (sp_set_stopbits(m_serialPort, 1) != SP_OK) configSuccess = false;
                       if (sp_set_rts(m_serialPort, SP_RTS_ON) != SP_OK) configSuccess = false;
                       if (sp_set_dtr(m_serialPort, SP_DTR_ON) != SP_OK) configSuccess = false;
                       
                       if (!configSuccess) {
                           sp_close(m_serialPort);
                           sp_free_port(m_serialPort);
                           m_serialPort = nullptr;
                       }
                   }
                   else
                   {
                       Log::Error(StringExtensions::Build("NamedPipeServer: Failed to reopen port for retry (Error: {0})", std::to_string((int)open_resp)));
                   }
               }
            }
#ifndef _WIN32
            else if (m_rawFd >= 0)
            {
               ssize_t written = write(m_rawFd, bytesToWrite.data(), bytesToWrite.size());
               if (written < 0) {
                   Log::Error(StringExtensions::Build("NamedPipeServer: Raw POSIX WRITE failed (errno: {0})", std::to_string(errno)));
                   close(m_rawFd);
                   m_rawFd = -1;
               }
            }
#endif

            std::string response = "OK";
#ifdef _WIN32
            HANDLE pipe = static_cast<HANDLE>(serverStream);
            DWORD bytesWritten;
            WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.length()), &bytesWritten, nullptr);
#else
            int sock = static_cast<int>(reinterpret_cast<intptr_t>(serverStream));
            ssize_t bytesWritten = write(sock, response.c_str(), response.length());
            (void)bytesWritten;
#endif
         }
         else if (StringExtensions::StartsWith(requestStr, "READLINE"))
         {
            std::string response;
            if (m_serialPort)
            {
               char buffer[256];
               int bytesRead = sp_blocking_read(m_serialPort, buffer, sizeof(buffer) - 1, 500);
               if (bytesRead > 0)
               {
                  buffer[bytesRead] = '\0';
                  response = buffer;
               }
            }

#ifdef _WIN32
            HANDLE pipe = static_cast<HANDLE>(serverStream);
            DWORD bytesWritten;
            WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.length()), &bytesWritten, nullptr);
#else
            int sock = static_cast<int>(reinterpret_cast<intptr_t>(serverStream));
            ssize_t bytesWritten = write(sock, response.c_str(), response.length());
            (void)bytesWritten;
#endif
         }
         else if (StringExtensions::StartsWith(requestStr, "CHECK"))
         {
            std::string response = "FALSE";
            if (m_serialPort)
            {
               void* handle;
               if (sp_get_port_handle(m_serialPort, &handle) == SP_OK && handle != nullptr)
               {
                  response = "TRUE";
               }
            }
#ifndef _WIN32
            else if (m_rawFd != -1)
            {
                response = "TRUE";
            }
#endif

#ifdef _WIN32
            HANDLE pipe = static_cast<HANDLE>(serverStream);
            DWORD bytesWritten;
            WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.length()), &bytesWritten, nullptr);
#else
            int sock = static_cast<int>(reinterpret_cast<intptr_t>(serverStream));
            ssize_t bytesWritten = write(sock, response.c_str(), response.length());
            (void)bytesWritten;
#endif
         }
         else if (StringExtensions::StartsWith(requestStr, "COMPORT"))
         {
#ifdef _WIN32
            HANDLE pipe = static_cast<HANDLE>(serverStream);
            DWORD bytesWritten;
            WriteFile(pipe, m_comPort.c_str(), static_cast<DWORD>(m_comPort.length()), &bytesWritten, nullptr);
#else
            int sock = static_cast<int>(reinterpret_cast<intptr_t>(serverStream));
            ssize_t bytesWritten = write(sock, m_comPort.c_str(), m_comPort.length());
            (void)bytesWritten;
#endif
         }
      }
      catch (...)
      {
         completed = true;
      }
   }
}

void NamedPipeServer::StopServer()
{
   m_isRunning = false;
#ifndef _WIN32
   if (m_serverSocket != -1)
   {
      shutdown(m_serverSocket, SHUT_RDWR);
      close(m_serverSocket);
      m_serverSocket = -1;
   }
#endif
   if (m_serverThread.joinable())
   {
      m_serverThread.join();
   }

   if (m_serialPort)
   {
      sp_close(m_serialPort);
   }
   CloseRawSerialPort();

   std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

bool NamedPipeServer::OpenRawSerialPort()
{
#ifdef _WIN32
   return false;
#else
   if (m_rawFd >= 0)
      return true;

   m_rawFd = open(m_comPort.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
   if (m_rawFd < 0)
   {
      Log::Error(StringExtensions::Build("NamedPipeServer: Raw POSIX open failed (errno: {0})", std::to_string(errno)));
      return false;
   }

   struct termios tty;
   if (tcgetattr(m_rawFd, &tty) != 0)
   {
      Log::Error("NamedPipeServer: Failed to get raw attributes");
      CloseRawSerialPort();
      return false;
   }

   cfmakeraw(&tty);
   tty.c_cflag |= (CLOCAL | CREAD);
   tty.c_cflag &= ~CRTSCTS;
   tty.c_cc[VMIN] = 0;
   tty.c_cc[VTIME] = 5;

   if (tcsetattr(m_rawFd, TCSANOW, &tty) != 0)
   {
      Log::Error("NamedPipeServer: Failed to set raw attributes");
      CloseRawSerialPort();
      return false;
   }

#ifdef __APPLE__
   // PinOne Mini macOS controller mode accepts output reports over this raw
   // serial path at 115200.
   speed_t speed = 115200;
   if (ioctl(m_rawFd, IOSSIOSPEED, &speed) == -1)
   {
      Log::Error("NamedPipeServer: Failed to set raw baud rate 115200");
      CloseRawSerialPort();
      return false;
   }
#else
   speed_t speed = B115200;
   switch (m_baudRate)
   {
   case 9600: speed = B9600; break;
   case 19200: speed = B19200; break;
   case 38400: speed = B38400; break;
   case 57600: speed = B57600; break;
   case 115200: speed = B115200; break;
   default:
      Log::Warning(StringExtensions::Build("NamedPipeServer: Unsupported POSIX baud rate {0}, using 115200", std::to_string(m_baudRate)));
      break;
   }
   cfsetospeed(&tty, speed);
   cfsetispeed(&tty, speed);
   tcsetattr(m_rawFd, TCSANOW, &tty);
#endif

   return true;
#endif
}

void NamedPipeServer::CloseRawSerialPort()
{
#ifndef _WIN32
   if (m_rawFd >= 0)
   {
      close(m_rawFd);
      m_rawFd = -1;
   }
#endif
}

}
