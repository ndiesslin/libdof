#include "DOF/DOF.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>

#include "DOF/Config.h"
#include "Log.h"
#include "Logger.h"
#include "Pinball.h"
#include "general/IOConfigurator.h"
#include "general/StringExtensions.h"

namespace DOF
{

namespace
{

std::string TrimRomName(std::string romName)
{
   romName.erase(romName.begin(), std::find_if(romName.begin(), romName.end(), [](unsigned char ch) {
      return !std::isspace(ch);
   }));
   romName.erase(std::find_if(romName.rbegin(), romName.rend(), [](unsigned char ch) {
      return !std::isspace(ch) && ch != ',' && ch != ';';
   }).base(), romName.end());
   return romName;
}

std::string ExtractAfterToken(const std::string& line, const std::string& token)
{
   const size_t tokenPos = line.find(token);
   if (tokenPos == std::string::npos)
      return {};

   const size_t valueStart = tokenPos + token.size();
   size_t valueEnd = line.find_first_of(", \t\r\n", valueStart);
   if (valueEnd == std::string::npos)
      valueEnd = line.size();

   return TrimRomName(line.substr(valueStart, valueEnd - valueStart));
}

std::string DetectRomFromVPinballLog()
{
   const char* home = std::getenv("HOME");
   if (!home || !*home)
      return {};

   const std::filesystem::path logPath = std::filesystem::path(home) / ".vpinball" / "vpinball.log";
   std::ifstream log(logPath);
   if (!log)
      return {};

   std::string line;
   std::string romName;
   while (std::getline(log, line))
   {
      std::string candidate = ExtractAfterToken(line, "put_GameName@697] newVal=");
      if (candidate.empty())
         candidate = ExtractAfterToken(line, "Game found: name=");

      if (!candidate.empty())
         romName = candidate;
   }

   return romName;
}

}

DOF::DOF()
{
   IOConfigurator::Initialize();
   m_pinball = new Pinball();
}

DOF::~DOF()
{
   delete m_pinball;
   IOConfigurator::Shutdown();
}

void DOF::Init(const char* tableFilename, const char* romName)
{
   Config* config = Config::GetInstance();

   const char* basePath = config->GetBasePath();
   std::string globalConfigPath;
   if (basePath && *basePath)
      globalConfigPath = std::string(basePath) + "directoutputconfig" + PATH_SEPARATOR_CHAR + "GlobalConfig_B2SServer.xml";

   if (std::filesystem::exists(globalConfigPath))
      Log::Write(StringExtensions::Build("Global configuration found at: {0}", globalConfigPath));
   else
   {
      const char* home = std::getenv("HOME");
      if (home && *home)
         globalConfigPath = std::string(home) + PATH_SEPARATOR_CHAR + ".vpinball" + PATH_SEPARATOR_CHAR + "directoutputconfig" + PATH_SEPARATOR_CHAR + "GlobalConfig_B2SServer.xml";

      if (std::filesystem::exists(globalConfigPath))
         Log::Write(StringExtensions::Build("Global configuration found at: {0}", globalConfigPath));
      else
      {
         globalConfigPath = std::string(".") + PATH_SEPARATOR_CHAR + "directoutputconfig" + PATH_SEPARATOR_CHAR + "GlobalConfig_B2SServer.xml";
         if (std::filesystem::exists(globalConfigPath))
            Log::Write(StringExtensions::Build("Global configuration found at: {0}", globalConfigPath));
      }

      if (!std::filesystem::exists(globalConfigPath))
      {
         globalConfigPath = (basePath && *basePath ? std::string(basePath) : std::string()) + "directoutputconfig" + PATH_SEPARATOR_CHAR + "GlobalConfig_B2SServer.xml";
         Log::Warning(StringExtensions::Build("Unable to find global configuration. Defaulting to: {0}", globalConfigPath));
      }
   }

   m_pinball->Setup(globalConfigPath, tableFilename, romName);
   m_pinball->Init();
   m_initialized = true;
}

void DOF::DataReceive(char type, int number, int value)
{
   if (!m_initialized)
   {
      const char* autoRom = std::getenv("LIBDOF_AUTO_ROM");
      std::string detectedRom;
      if (!autoRom || !*autoRom)
      {
         detectedRom = DetectRomFromVPinballLog();
         autoRom = detectedRom.c_str();
      }

      Init("", autoRom && *autoRom ? autoRom : "");
   }
   m_pinball->ReceiveData(type, number, value);
}

void DOF::Finish()
{
   if (m_initialized)
   {
      m_pinball->Finish();
      m_initialized = false;
   }
}

}
