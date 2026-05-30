#pragma once

#include "DOF/DOF.h"

#ifdef __LIBUSB__
#include <libusb-1.0/libusb.h>
#endif

namespace DOF
{

class IOConfigurator
{
public:
   static void Initialize();
   static void Shutdown();

#ifdef __LIBUSB__
   static bool InitializeUSB();
   static libusb_context* GetUSBContext() { return s_libusbContext; }
#endif

private:
   IOConfigurator() = delete;

#ifdef __LIBUSB__
   static libusb_context* s_libusbContext;
#endif
};

}
